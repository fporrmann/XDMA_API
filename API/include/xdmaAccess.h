/* 
 *  File: xdmaAccess.h
 *  Copyright (c) 2021 Florian Porrmann
 *  
 *  MIT License
 *  
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *  
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *  
 */

#pragma once

////// TODO:
// - Maybe change the way IP core objects are created, possible to create from an XDMA object?
// --------------------------------------------------------------------------------------------
// - Rename this file to something more appropriate
// --------------------------------------------------------------------------------------------
// - Look into 32-bit AXI interfaces, although this is disabled by default it might still be used
//   - Would require some edits to the memory manager
//   - Would require that the DDR address is below 4GB
// --------------------------------------------------------------------------------------------
// - Determine if the core is setup as memory mapped or streaming
//   - from xdma0_control read 0x0H00 and check if it starts with 1fc08 - the 8 here means it's in streaming mode
//   - cf. https://github.com/Xilinx/dma_ip_drivers/blob/master/XDMA/linux-kernel/tests/run_test.sh)
// --------------------------------------------------------------------------------------------
// - Try to force the use of aligned memory, e.g., by using the DMABuffer type, alternative force vector types to be aligned with the xdmaAlignmentAllocator
//   - Maybe prevent passing for custom memory addresses alltogether
// --------------------------------------------------------------------------------------------
// - Replace boolean flags with enums for better readability
// --------------------------------------------------------------------------------------------
// - Improve / generalize the CMake environment for the samples
// --------------------------------------------------------------------------------------------
// - Redesign some of the methods to remove the need for explizit casts
/////////////////////////
// Includes for open()
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
/////////////////////////

#ifndef EMBEDDED_XILINX
#ifndef _WIN32
/////////////////////////
// Include for mmap(), munmap()
#include <sys/mman.h>
/////////////////////////
#endif
#endif

#include <algorithm>
#include <cstring> // required for std::memcpy
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include "internal/Constants.h"
#include "internal/Memory.h"
#include "internal/Utils.h"

#ifndef EMBEDDED_XILINX
#include "internal/Timer.h"
#include "internal/xdmaAlignmentAllocator.h"
#endif

#include "internal/xdmaBackend.h"

#ifdef EMBEDDED_XILINX
using DMABuffer = std::vector<uint8_t>;
#else
using DMABuffer = std::vector<uint8_t, xdma::AlignmentAllocator<uint8_t, XDMA_ALIGNMENT>>;
#endif
using XDMAManagedShr   = std::shared_ptr<class XDMAManaged>;
using XDMABackendShr   = std::shared_ptr<class XDMABackend>;
using XDMAShr          = std::shared_ptr<class XDMA>;
using MemoryManagerShr = std::shared_ptr<MemoryManager>;
using MemoryManagerVec = std::vector<MemoryManagerShr>;

class XDMAManaged
{
	friend class XDMABase;
	DISABLE_COPY_ASSIGN_MOVE(XDMAManaged)

protected:
	XDMAManaged(std::shared_ptr<class XDMABase> pXdma);

	~XDMAManaged();

	std::shared_ptr<class XDMABase> XDMA()
	{
		checkXDMAValid();
		return m_pXdma;
	}

private:
	void markXDMAInvalid()
	{
		m_pXdma = nullptr;
	}

	void checkXDMAValid() const
	{
		if (m_pXdma == nullptr)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAManaged") << "XDMA/XDMAPio instance is not valid or has been destroyed";
			throw XDMAException(ss.str());
		}
	}

private:
	std::shared_ptr<class XDMABase> m_pXdma;
};

class XDMABase
{
	friend class XDMAManaged;

public:
	virtual uint8_t Read8(const uint64_t& addr)   = 0;
	virtual uint16_t Read16(const uint64_t& addr) = 0;
	virtual uint32_t Read32(const uint64_t& addr) = 0;
	virtual uint64_t Read64(const uint64_t& addr) = 0;

	virtual void Write8(const uint64_t& addr, const uint8_t& data)   = 0;
	virtual void Write16(const uint64_t& addr, const uint16_t& data) = 0;
	virtual void Write32(const uint64_t& addr, const uint32_t& data) = 0;
	virtual void Write64(const uint64_t& addr, const uint64_t& data) = 0;

	uint32_t GetDevNum() const
	{
		return m_devNum;
	}

protected:
	XDMABase(const uint32_t& devNum) :
		m_devNum(devNum),
		m_managedObjects()
	{
	}

	virtual ~XDMABase()
	{
		for (XDMAManaged* pM : m_managedObjects)
			pM->markXDMAInvalid();
	}

private:
	void registerObject(XDMAManaged* pObj)
	{
		m_managedObjects.push_back(pObj);
	}

	void unregisterObject(XDMAManaged* pObj)
	{
		m_managedObjects.erase(std::remove(m_managedObjects.begin(), m_managedObjects.end(), pObj), m_managedObjects.end());
	}

protected:
	uint32_t m_devNum;
	std::vector<XDMAManaged*> m_managedObjects;
};

class XDMA : virtual public XDMABase
{
public:
	enum class MemoryType
	{
		DDR,
		BRAM
	};

private:
	using MemoryPair = std::pair<MemoryType, MemoryManagerVec>;

	struct XDMAInfo
	{
		XDMAInfo(const uint32_t& reg0x0, const uint32_t& reg0x4)
		{
			channelID = (reg0x0 >> 8) & 0xF;
			version   = (reg0x0 >> 0) & 0xF;
			streaming = (reg0x0 >> 15) & 0x1;
			polling   = (reg0x4 >> 26) & 0x1;
		}

		friend std::ostream& operator<<(std::ostream& os, const XDMAInfo& info)
		{
			os << "Channel ID: " << static_cast<uint32_t>(info.channelID) << std::endl;
			os << "Version: " << static_cast<uint32_t>(info.version) << std::endl;
			os << "Streaming: " << (info.streaming ? "true" : "false") << std::endl;
			os << "Polling: " << (info.polling ? "true" : "false");
			return os;
		}

		uint8_t channelID = 0;
		uint8_t version = 0;
		bool streaming = false;
		bool polling = false;
	};

	XDMA(XDMABackendShr pBackend) :
		XDMABase(pBackend->GetDevNum()),
		m_pBackend(pBackend),
		m_memories()
#ifndef EMBEDDED_XILINX
		,
		m_mutex()
#endif
	{
		m_memories.insert(MemoryPair(MemoryType::DDR, MemoryManagerVec()));
		m_memories.insert(MemoryPair(MemoryType::BRAM, MemoryManagerVec()));

		LOG_VERBOSE << "XDMA instance created" << std::endl;
		LOG_VERBOSE << "Device number: " << m_devNum << std::endl;
		LOG_VERBOSE << "Backend: " << m_pBackend->GetBackendName() << std::endl;
		LOG_VERBOSE << readInfo() << std::endl;
	}

public:
	/// @brief Creates a new XDMA instance
	/// @tparam T Type of the backend to use
	/// @return A shared pointer to the new XDMA instance
	template<typename T>
	static XDMAShr Create(const uint32_t& deviceNum = 0, const uint32_t& channelNum = 0)
	{
		// We have to use the result of new here, because the constructor is private
		// and can therefore, not be called from make_shared
		return XDMAShr(new XDMA(std::make_shared<T>(deviceNum, channelNum)));
	}

	~XDMA()
	{
	}

	/// @brief Adds a memory region to the XDMA instance
	/// @param type Type of memory
	/// @param baseAddr Base address of the memory region
	/// @param size Size of the memory region in bytes
	void AddMemoryRegion(const MemoryType& type, const uint64_t& baseAddr, const uint64_t& size)
	{
		m_memories[type].push_back(std::make_shared<MemoryManager>(baseAddr, size));
	}

	/// @brief Allocates a memory block of the specified size and type
	/// @param type	Type of memory to allocate
	/// @param byteSize	Size of the memory block in bytes
	/// @param memIdx Index of the memory region to allocate from
	/// @return Allocated memory block
	Memory AllocMemory(const MemoryType& type, const uint64_t& byteSize, const int32_t& memIdx = -1)
	{
#ifndef EMBEDDED_XILINX
		std::lock_guard<std::mutex> lock(m_mutex);
#endif

		if (memIdx == -1)
		{
			for (MemoryManagerShr& mem : m_memories[type])
			{
				if (mem->GetAvailableSpace() >= byteSize)
					return mem->AllocMemory(byteSize);
			}
		}
		else
		{
			if (m_memories[type].size() <= static_cast<uint32_t>(memIdx))
			{
				std::stringstream ss;
				ss << CLASS_TAG("XDMA") << "Specified memory region " << std::dec << memIdx << " does not exist.";
				throw XDMAException(ss.str());
			}

			return m_memories[type][memIdx]->AllocMemory(byteSize);
		}

		std::stringstream ss;
		ss << CLASS_TAG("XDMA") << "No memory region found with enough space left to allocate " << std::dec << byteSize << " byte.";
		throw XDMAException(ss.str());
	}

	/// @brief Allocates a memory block of the specified size and type
	/// @param type Type of memory to allocate
	/// @param elements Number of elements to allocate
	/// @param sizeOfElement Size of one element in bytes
	/// @param memIdx Index of the memory region to allocate from
	/// @return Allocated memory block
	Memory AllocMemory(const MemoryType& type, const uint64_t& elements, const uint64_t& sizeOfElement, const int32_t& memIdx = -1)
	{
		return AllocMemory(type, elements * sizeOfElement, memIdx);
	}

	/// @brief Allocates a DDR memory block of the specified size
	/// @param elements Number of elements to allocate
	/// @param sizeOfElement Size of one element in bytes
	/// @param memIdx Index of the memory region to allocate from
	/// @return Allocated DDR memory block
	Memory AllocMemoryDDR(const uint64_t& elements, const uint64_t& sizeOfElement, const int32_t& memIdx = -1)
	{
		return AllocMemory(MemoryType::DDR, elements, sizeOfElement, memIdx);
	}

	/// @brief Allocates a BRAM memory block of the specified size
	/// @param elements Number of elements to allocate
	/// @param sizeOfElement Size of one element in bytes
	/// @param memIdx Index of the memory region to allocate from
	/// @return Allocated BRAM memory block
	Memory AllocMemoryBRAM(const uint64_t& elements, const uint64_t& sizeOfElement, const int32_t& memIdx = -1)
	{
		return AllocMemory(MemoryType::BRAM, elements, sizeOfElement, memIdx);
	}

	/// @brief Allocates a DDR memory block of the specified byte size
	/// @param byteSize Size of the memory block in bytes
	/// @param memIdx Index of the memory region to allocate from
	/// @return Allocated DDR memory block
	Memory AllocMemoryDDR(const uint64_t& byteSize, const int32_t& memIdx = -1)
	{
		return AllocMemory(MemoryType::DDR, byteSize, memIdx);
	}

	/// @brief Allocates a BRAM memory block of the specified byte size
	/// @param byteSize Size of the memory block in bytes
	/// @param memIdx Index of the memory region to allocate from
	/// @return Allocated BRAM memory block
	Memory AllocMemoryBRAM(const uint64_t& byteSize, const int32_t& memIdx = -1)
	{
		return AllocMemory(MemoryType::BRAM, byteSize, memIdx);
	}

	////////////////////////////////////////////////////////////////////////////
	///                      Read Methods                                    ///
	////////////////////////////////////////////////////////////////////////////

	/// @brief Reads data from the specified address into the data buffer
	/// @param addr Address to read from
	/// @param pData Pointer to the data buffer
	/// @param sizeInByte Size of the data buffer in bytes
	void Read(const uint64_t& addr, void* pData, const uint64_t& sizeInByte)
	{
		m_pBackend->Read(addr, pData, sizeInByte);
	}

	/// @brief Reads data from the specified memory object into the data buffer
	/// @param mem Memory object to read from
	/// @param pData Pointer to the data buffer
	/// @param sizeInByte Size of the data buffer in bytes
	void Read(const Memory& mem, void* pData, const uint64_t& sizeInByte = USE_MEMORY_SIZE)
	{
		uint64_t size = (sizeInByte == USE_MEMORY_SIZE ? mem.GetSize() : sizeInByte);
		if (size > mem.GetSize())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << m_pBackend->GetName(XDMABackend::TYPE::READ) << ", specified size (0x" << std::hex << size << ") exceeds size of the given buffer (0x" << std::hex << mem.GetSize() << ")";
			throw XDMAException(ss.str());
		}

		Read(mem.GetBaseAddr(), pData, size);
	}

	/// @brief Reads data from the specified address into the given DMA buffer
	/// @param addr Address to read from
	/// @param buffer DMA buffer to read into
	/// @param sizeInByte Size of the data buffer in bytes
	void Read(const uint64_t& addr, DMABuffer& buffer, const uint64_t& sizeInByte)
	{
		if (sizeInByte > buffer.size())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Size of buffer provided (" << std::dec << buffer.size() << ") is smaller than the desired read size (" << sizeInByte << ")";
			throw XDMAException(ss.str());
		}

		Read(addr, buffer.data(), sizeInByte);
	}

	/// @brief Reads data from the specified address into the given DMA buffer
	/// @param addr Address to read from
	/// @param buffer DMA buffer to read into
	void Read(const uint64_t& addr, DMABuffer& buffer)
	{
		Read(addr, buffer.data(), buffer.size());
	}

	/// @brief Reads data from the specified address and returns it as a DMA buffer
	/// @param addr Address to read from
	/// @param sizeInByte Size of the data buffer in bytes
	/// @return DMA buffer containing the read data
	DMABuffer Read(const uint64_t& addr, const uint32_t& sizeInByte) CHECK_RESULT
	{
		DMABuffer buffer = DMABuffer(sizeInByte, 0);
		Read(addr, buffer, sizeInByte);
		return buffer;
	}

	/// @brief Reads data from the specified address and returns it as an object of the template type T
	/// @tparam T Object type of the object into which the data will be read
	/// @param addr Address to read from
	/// @return Object of type T containing the read data
	template<typename T>
	T Read(const uint64_t& addr)
	{
		const uint32_t size = static_cast<uint32_t>(sizeof(T));
		DMABuffer data      = Read(addr, size);
		T res;
		std::memcpy(&res, data.data(), size);
		return res;
	}

	/// @brief Reads data from the specified address and writes it into the given object
	/// @tparam T Object type of the object into which the data will be read
	/// @param addr Address to read from
	/// @param buffer Object into which the data will be read
	template<typename T>
	void Read(const uint64_t& addr, T& buffer)
	{
		const uint64_t size = static_cast<uint64_t>(sizeof(T));
		DMABuffer data      = Read(addr, size);
		std::memcpy(&buffer, data.data(), size);
	}

	/// @brief Reads data from the specified address and writes it into the given alligned vector
	/// @tparam T Object type of the vector into which the data will be read
	/// @tparam A The allocator used for the vector
	/// @param addr Address to read from
	/// @param data Vector into which the data will be read
	template<class T, class A = xdma::AlignmentAllocator<T, XDMA_ALIGNMENT>>
	void Read(const uint64_t& addr, std::vector<T, A>& data)
	{
		std::size_t size = sizeof(T);
		Read(addr, data.data(), data.size() * size);
	}

	/// @brief Reads a single unsigned byte from the specified address
	/// @param addr Address to read from
	/// @return Unsigned byte read from the specified address
	uint8_t Read8(const uint64_t& addr)
	{
		return Read<uint8_t>(addr);
	}

	/// @brief Reads a single unsigned word from the specified address
	/// @param addr Address to read from
	/// @return Unsigned word read from the specified address
	uint16_t Read16(const uint64_t& addr)
	{
		return Read<uint16_t>(addr);
	}

	/// @brief Reads a single unsigned double word from the specified address
	/// @param addr Address to read from
	/// @return Unsigned double word read from the specified address
	uint32_t Read32(const uint64_t& addr)
	{
		return Read<uint32_t>(addr);
	}

	/// @brief Reads a single unsigned quad word from the specified address
	/// @param addr Address to read from
	/// @return Unsigned quad word read from the specified address
	uint64_t Read64(const uint64_t& addr)
	{
		return Read<uint64_t>(addr);
	}

	/// @brief Reads a single unsigned byte from the specified memory object
	/// @param mem Memory object to read from
	/// @return Unsigned byte read from the specified memory object
	uint8_t Read8(const Memory& mem)
	{
		return read<uint8_t>(mem);
	}

	/// @brief Reads a single unsigned word from the specified memory object
	/// @param mem Memory object to read from
	/// @return Unsigned word read from the specified memory object
	uint16_t Read16(const Memory& mem)
	{
		return read<uint16_t>(mem);
	}

	/// @brief Reads a single unsigned double word from the specified memory object
	/// @param mem Memory object to read from
	/// @return Unsigned double word read from the specified memory object
	uint32_t Read32(const Memory& mem)
	{
		return read<uint32_t>(mem);
	}

	/// @brief Reads a single unsigned quad word from the specified memory object
	/// @param mem Memory object to read from
	/// @return Unsigned quad word read from the specified memory object
	uint64_t Read64(const Memory& mem)
	{
		return read<uint64_t>(mem);
	}

	////////////////////////////////////////////////////////////////////////////
	///                      Write Methods                                   ///
	////////////////////////////////////////////////////////////////////////////

	/// @brief Writes data to the specified address
	/// @param addr Address to write to
	/// @param pData Pointer to the data buffer
	/// @param sizeInByte Size of the data buffer in bytes
	void Write(const uint64_t& addr, const void* pData, const uint64_t& sizeInByte)
	{
		m_pBackend->Write(addr, pData, sizeInByte);
	}

	/// @brief Writes data to the specified memory object
	/// @param mem Memory object to write to
	/// @param pData Pointer to the data buffer
	/// @param sizeInByte Size of the data buffer in bytes
	void Write(const Memory& mem, const void* pData, const uint64_t& sizeInByte = USE_MEMORY_SIZE)
	{
		uint64_t size = (sizeInByte == USE_MEMORY_SIZE ? mem.GetSize() : sizeInByte);
		if (size > mem.GetSize())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << m_pBackend->GetName(XDMABackend::TYPE::WRITE) << ", specified size (0x" << std::hex << size << ") exceeds size of the given buffer (0x" << std::hex << mem.GetSize() << ")";
			throw XDMAException(ss.str());
		}

		Write(mem.GetBaseAddr(), pData, size);
	}

	/// @brief Writes data to the specified address
	/// @param addr Address to write to
	/// @param pData Pointer to the data buffer
	/// @param sizeInByte Size of the data buffer in bytes
	void Write(const uint64_t& addr, const DMABuffer& buffer, const uint64_t& sizeInByte)
	{
		if (sizeInByte > buffer.size())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Size of buffer provided (" << std::dec << buffer.size() << ") is smaller than the desired write size (" << sizeInByte << ")";
			throw XDMAException(ss.str());
		}

		Write(addr, buffer.data(), sizeInByte);
	}

	/// @brief Writes data to the specified address
	/// @param addr Address to write to
	/// @param pData Pointer to the data buffer
	/// @param sizeInByte Size of the data buffer in bytes
	void Write(const uint64_t& addr, const DMABuffer& buffer)
	{
		Write(addr, buffer.data(), buffer.size());
	}

	/// @brief Writes data to the specified address
	/// @tparam T Type of the data to write
	/// @param addr Address to write to
	/// @param data Data to write to the specified address
	template<typename T>
	void Write(const uint64_t& addr, const T& data)
	{
		//  === Ugly Workaround ===
		// If a global const variable was passed as data argument
		// the write call fails, to circumvent this a local copy
		// is created and passed to the underlying method
		const T tmp = data;
		//  === Ugly Workaround ===
		Write(addr, reinterpret_cast<const void*>(&tmp), sizeof(T));
	}

	/// @brief Writes data from a vector to the specified address
	/// @tparam T Object type of the vector from which the data will be written
	/// @tparam A The allocator used for the vector
	/// @param addr Address to write to
	/// @param data Vector containing the data to write to the specified address
	template<class T, class A = std::allocator<T>>
	void Write(const uint64_t& addr, const std::vector<T, A>& data)
	{
		Write(addr, data.data(), data.size() * sizeof(T));
	}

	/// @brief Writes a single unsigned byte to the specified address
	/// @param addr Address to write to
	/// @param data Unsigned byte to write to the specified address
	void Write8(const uint64_t& addr, const uint8_t& data)
	{
		Write<uint8_t>(addr, data);
	}

	/// @brief Writes a single unsigned word to the specified address
	/// @param addr Address to write to
	/// @param data Unsigned word to write to the specified address
	void Write16(const uint64_t& addr, const uint16_t& data)
	{
		Write<uint16_t>(addr, data);
	}

	/// @brief Writes a single unsigned double word to the specified address
	/// @param addr Address to write to
	/// @param data Unsigned double word to write to the specified address
	void Write32(const uint64_t& addr, const uint32_t& data)
	{
		Write<uint32_t>(addr, data);
	}

	/// @brief Writes a single unsigned quad word to the specified address
	/// @param addr Address to write to
	/// @param data Unsigned quad word to write to the specified address
	void Write64(const uint64_t& addr, const uint64_t& data)
	{
		Write<uint64_t>(addr, data);
	}

	/// @brief Writes a single unsigned byte to the specified memory object
	/// @param mem Memory object to write to
	/// @param data Unsigned byte to write to the specified memory object
	void Write8(const Memory& mem, const uint8_t& data)
	{
		write<uint8_t>(mem, data);
	}

	/// @brief Writes a single unsigned word to the specified memory object
	/// @param mem Memory object to write to
	/// @param data Unsigned word to write to the specified memory object
	void Write16(const Memory& mem, const uint16_t& data)
	{
		write<uint16_t>(mem, data);
	}

	/// @brief Writes a single unsigned double word to the specified memory object
	/// @param mem Memory object to write to
	/// @param data Unsigned double word to write to the specified memory object
	void Write32(const Memory& mem, const uint32_t& data)
	{
		write<uint32_t>(mem, data);
	}

	/// @brief Writes a single unsigned quad word to the specified memory object
	/// @param mem Memory object to write to
	/// @param data Unsigned quad word to write to the specified memory object
	void Write64(const Memory& mem, const uint64_t& data)
	{
		write<uint64_t>(mem, data);
	}

	////////////////////////////////////////////////////////////////////////////
	///                      Streaming Methods                               ///
	////////////////////////////////////////////////////////////////////////////

	/// @brief Starts a streaming read, reading sizeInByte bytes into the specified DMA buffer
	/// @param buffer DMA buffer to read into
	/// @param sizeInByte Number of bytes to read
	void StartReadStream(DMABuffer& buffer, const uint64_t& sizeInByte = USE_VECTOR_SIZE)
	{
		uint64_t size = (sizeInByte == USE_VECTOR_SIZE ? buffer.size() * sizeof(DMABuffer::value_type) : sizeInByte);
		startReadStream(buffer.data(), size);
	}

	/// @brief Starts a streaming read, reading sizeInByte bytes into the specified vector
	/// @tparam T Object type of the vector into which the data will be read
	/// @tparam A The allocator used for the vector
	/// @param buffer Vector to read into
	/// @param sizeInByte Number of bytes to read
	template<class T, class A = xdma::AlignmentAllocator<T, XDMA_ALIGNMENT>>
	void StartReadStream(std::vector<T, A>& buffer, const uint64_t& sizeInByte = USE_VECTOR_SIZE)
	{
		uint64_t size = (sizeInByte == USE_VECTOR_SIZE ? buffer.size() * sizeof(T) : sizeInByte);
		startReadStream(buffer.data(), size);
	}

	/// @brief Starts a streaming write, writing sizeInByte bytes from the specified DMA buffer
	/// @param buffer DMA buffer containing the data to write
	/// @param sizeInByte Number of bytes to write
	void StartWriteStream(const DMABuffer& buffer, const uint64_t& sizeInByte = USE_VECTOR_SIZE)
	{
		uint64_t size = (sizeInByte == USE_VECTOR_SIZE ? buffer.size() * sizeof(DMABuffer::value_type) : sizeInByte);
		startWriteStream(buffer.data(), size);
	}

	/// @brief Starts a streaming write, writing sizeInByte bytes from the specified vector
	/// @tparam T Object type of the vector from which the data will be written
	/// @tparam A The allocator used for the vector
	/// @param buffer Vector containing the data to write
	/// @param sizeInByte Number of bytes to write
	template<class T, class A = xdma::AlignmentAllocator<T, XDMA_ALIGNMENT>>
	void StartWriteStream(const std::vector<T, A>& buffer, const uint64_t& sizeInByte = USE_VECTOR_SIZE)
	{
		uint64_t size = (sizeInByte == USE_VECTOR_SIZE ? buffer.size() * sizeof(T) : sizeInByte);
		startWriteStream(buffer.data(), size);
	}

	/// @brief Waits for the read stream operation to finish
	void WaitForReadStream()
	{
		if (m_readFuture.valid())
		{
			m_readFuture.wait();
			m_readFuture.get();
		}
	}

	/// @brief Waits for the write stream operation to finish
	void WaitForWriteStream()
	{
		if (m_writeFuture.valid())
		{
			m_writeFuture.wait();
			m_writeFuture.get();
		}
	}

	/// @brief Waits for the read and write stream operations to finish
	void WaitForStreams()
	{
		WaitForReadStream();
		WaitForWriteStream();
	}

	/// @brief Returns the runtime of the last read stream operation in milliseconds
	/// @return Runtime in milliseconds
	double GetReadStreamRuntime() const
	{
#ifndef EMBEDDED_XILINX
		return m_readStreamTimer.GetElapsedTimeInMilliSec();
#else
		return 0.0; // TODO: Add embedded runtime measurement
#endif
	}

	/// @brief Returns the runtime of the last write stream operation in milliseconds
	/// @return Runtime in milliseconds
	double GetWriteStreamRuntime() const
	{
#ifndef EMBEDDED_XILINX
		return m_writeStreamTimer.GetElapsedTimeInMilliSec();
#else
		return 0.0; // TODO: Add embedded runtime measurement
#endif
	}

private:
	template<typename T>
	T read(const Memory& mem)
	{
		if (sizeof(T) > mem.GetSize())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Size of provided memory (" << std::dec << mem.GetSize() << ") is smaller than the desired read size (" << sizeof(T) << ")";
			throw XDMAException(ss.str());
		}

		return Read<T>(mem.GetBaseAddr());
	}

	template<typename T>
	void write(const Memory& mem, const T& data)
	{
		if (sizeof(T) > mem.GetSize())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Size of provided memory (" << std::dec << mem.GetSize() << ") is smaller than the desired write size (" << sizeof(T) << ")";
			throw XDMAException(ss.str());
		}

		return Write<T>(mem.GetBaseAddr(), data);
	}

	void startReadStream(void* pData, const uint64_t& sizeInByte)
	{
		if (m_readFuture.valid())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Read stream is already running";
			throw XDMAException(ss.str());
		}

		m_readFuture = std::async(&XDMA::readStream, this, pData, sizeInByte);
	}

	void startWriteStream(const void* pData, const uint64_t& sizeInByte)
	{
		if (m_writeFuture.valid())
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Write stream is already running";
			throw XDMAException(ss.str());
		}

		m_writeFuture = std::async(&XDMA::writeStream, this, pData, sizeInByte);
	}

	void writeStream(const void* pData, const uint64_t& size)
	{
		// Due to the AXI data width of the XDMA write size has to be a multiple of 512-Bit (64-Byte)
		if (size % XDMA_AXI_DATA_WIDTH != 0)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Size (" << size << ") is not a multiple of the XDMA AXI data width (" << XDMA_AXI_DATA_WIDTH << ").";
			throw XDMAException(ss.str());
		}

		uint64_t curSize = 0;
		m_writeStreamTimer.Start();

		while (curSize < size)
		{
			uint64_t writeSize = std::min(size - curSize, static_cast<uint64_t>(XDMA_ALIGNMENT));
			Write(XDMA_STREAM_OFFSET, reinterpret_cast<const uint8_t*>(pData) + curSize, writeSize);
			curSize += writeSize;
		}

		m_writeStreamTimer.Stop();
	}

	void readStream(void* pData, const uint64_t& size)
	{
		// Due to the AXI data width of the XDMA read size has to be a multiple of 512-Bit (64-Byte)
		if (size % XDMA_AXI_DATA_WIDTH != 0)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMA") << "Size (" << size << ") is not a multiple of the XDMA AXI data width (" << XDMA_AXI_DATA_WIDTH << ").";
			throw XDMAException(ss.str());
		}

		uint64_t curSize = 0;
		m_readStreamTimer.Start();

		while (curSize < size)
		{
			uint64_t readSize = std::min(size - curSize, static_cast<uint64_t>(XDMA_ALIGNMENT));
			Read(XDMA_STREAM_OFFSET, reinterpret_cast<uint8_t*>(pData) + curSize, readSize);
			curSize += readSize;
		}

		m_readStreamTimer.Stop();
	}

	// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

	uint8_t readCtrl8(const uint64_t& addr)
	{
		return readCtrl<uint8_t>(addr);
	}

	uint16_t readCtrl16(const uint64_t& addr)
	{
		return readCtrl<uint16_t>(addr);
	}

	uint32_t readCtrl32(const uint64_t& addr)
	{
		return readCtrl<uint32_t>(addr);
	}

	uint64_t readCtrl64(const uint64_t& addr)
	{
		return readCtrl<uint64_t>(addr);
	}

	template<typename T>
	T readCtrl(const uint64_t& addr)
	{
		uint64_t tmp;
		m_pBackend->ReadCtrl(addr, tmp, sizeof(T));
		return static_cast<T>(tmp);
	}

	XDMAInfo readInfo()
	{
		uint32_t reg0 = readCtrl32(XDMA_CTRL_BASE + m_devNum * XDMA_CTRL_SIZE + 0x0);
		uint32_t reg4 = readCtrl32(XDMA_CTRL_BASE + m_devNum * XDMA_CTRL_SIZE + 0x4);
		return XDMAInfo(reg0, reg4);
	}
	// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

private:
	XDMABackendShr m_pBackend;
	std::map<MemoryType, MemoryManagerVec> m_memories;
	std::future<void> m_readFuture  = {};
	std::future<void> m_writeFuture = {};
	xdma::Timer m_readStreamTimer   = {};
	xdma::Timer m_writeStreamTimer  = {};
#ifndef EMBEDDED_XILINX
	std::mutex m_mutex;
#endif
};

#ifndef _WIN32
// TODO: Add backend classes for Pio
// NOTE: PIO is used for the AXI-Lite interface of the XDMA -- But currently not fully supported by this API
class XDMAPio : virtual public XDMABase
{
public:
	XDMAPio(const uint32_t& deviceNum, const std::size_t& pioSize, const std::size_t& pioOffset = 0) :
		XDMABase(deviceNum),
		m_pioDeviceName("/dev/xdma" + std::to_string(deviceNum) + "_user"),
		m_pioSize(pioSize),
		m_pioOffset(pioOffset)
#ifndef EMBEDDED_XILINX
		,
		m_mutex()
#endif
	{
		m_fd        = open(m_pioDeviceName.c_str(), O_RDWR | O_NONBLOCK);
		int32_t err = errno;

		if (m_fd < 0)
		{
			std::stringstream ss;
			ss << CLASS_TAG("") << "Unable to open device " << m_pioDeviceName << "; errno: " << err;
			throw XDMAException(ss.str());
		}

#ifndef EMBEDDED_XILINX
		m_pMapBase = mmap(0, m_pioSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, m_pioOffset);
		err        = errno;

		if (m_pMapBase == MAP_FAILED)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "Failed to map memory into userspace, errno: " << err;
			throw XDMAException(ss.str());
		}
#endif

		m_valid = true;
	}

	DISABLE_COPY_ASSIGN_MOVE(XDMAPio)

	~XDMAPio()
	{
#ifndef EMBEDDED_XILINX
		std::lock_guard<std::mutex> lock(m_mutex);

		munmap(m_pMapBase, m_pioSize);
#endif
		close(m_fd);
	}

	uint8_t Read8(const uint64_t& addr)
	{
		return read<uint8_t>(addr);
	}

	uint16_t Read16(const uint64_t& addr)
	{
		return read<uint16_t>(addr);
	}

	uint32_t Read32(const uint64_t& addr)
	{
		return read<uint32_t>(addr);
	}

	uint64_t Read64(const uint64_t& addr)
	{
		return read<uint64_t>(addr);
	}

	void Write8(const uint64_t& addr, const uint8_t& data)
	{
		write<uint8_t>(addr, data);
	}

	void Write16(const uint64_t& addr, const uint16_t& data)
	{
		write<uint16_t>(addr, data);
	}

	void Write32(const uint64_t& addr, const uint32_t& data)
	{
		write<uint32_t>(addr, data);
	}

	void Write64(const uint64_t& addr, const uint64_t& data)
	{
		write<uint64_t>(addr, data);
	}

private:
	template<typename T>
	T read(const uint64_t& addr)
	{
#ifndef EMBEDDED_XILINX
		std::lock_guard<std::mutex> lock(m_mutex);
#endif

		if (!m_valid)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "XDMAPio Instance is not valid, an error probably occurred during device initialization.";
			throw XDMAException(ss.str());
		}

		const std::size_t size = sizeof(T);
		if (size > MAX_PIO_ACCESS_SIZE)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "Type size (" << std::dec << size << " byte) exceeds maximal allowed Pio size (" << MAX_PIO_ACCESS_SIZE << " byte)";
			throw XDMAException(ss.str());
		}

		if (addr >= m_pioSize + m_pioOffset)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "Address: (0x" << std::hex << addr << ") exceeds Pio address range (0x" << m_pioOffset << "-0x" << m_pioSize + m_pioOffset << ")";
			throw XDMAException(ss.str());
		}

		uint8_t* vAddr = reinterpret_cast<uint8_t*>(m_pMapBase) + addr;
		T result       = *(reinterpret_cast<T*>(vAddr));
		return result;
	}

	template<typename T>
	void write(const uint64_t& addr, const T& data)
	{
#ifndef EMBEDDED_XILINX
		std::lock_guard<std::mutex> lock(m_mutex);
#endif

		if (!m_valid)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "XDMAPio Instance is not valid, an error probably occurred during device initialization.";
			throw XDMAException(ss.str());
		}

		const std::size_t size = sizeof(T);
		if (size > MAX_PIO_ACCESS_SIZE)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "Type size (" << std::dec << size << " byte) exceeds maximal allowed Pio size (" << MAX_PIO_ACCESS_SIZE << " byte)";
			throw XDMAException(ss.str());
		}

		if (addr >= m_pioSize + m_pioOffset)
		{
			std::stringstream ss;
			ss << CLASS_TAG("XDMAPio") << "Address (0x" << std::hex << addr << ") exceeds Pio address range (0x" << m_pioOffset << "-0x" << m_pioSize + m_pioOffset << ")";
			throw XDMAException(ss.str());
		}

		uint8_t* vAddr                 = reinterpret_cast<uint8_t*>(m_pMapBase) + addr;
		*(reinterpret_cast<T*>(vAddr)) = data;
	}

private:
	std::string m_pioDeviceName;
	std::size_t m_pioSize;
	std::size_t m_pioOffset;
	int32_t m_fd     = -1;
	void* m_pMapBase = nullptr;
	bool m_valid     = false;
#ifndef EMBEDDED_XILINX
	std::mutex m_mutex;
#endif

	static const std::size_t MAX_PIO_ACCESS_SIZE = sizeof(uint64_t);
};
#endif // XDMAPio

inline XDMAManaged::XDMAManaged(std::shared_ptr<XDMABase> pXdma) :
	m_pXdma(pXdma)
{
	if (m_pXdma)
		m_pXdma->registerObject(this);
}

inline XDMAManaged::~XDMAManaged()
{
	if (m_pXdma)
		m_pXdma->unregisterObject(this);
}
