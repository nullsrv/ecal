/* ========================= eCAL LICENSE =================================
 *
 * Copyright (C) 2016 - 2019 Continental Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ========================= eCAL LICENSE =================================
*/

/**
 * @brief  synchronized memory file interface
**/

#include <ecal/ecal_event.h>
#include <ecal/ecal_log.h>

#include "ecal_memfile_header.h"
#include "ecal_memfile_naming.h"
#include "ecal_memfile_sync.h"

#include <chrono>
#include <sstream>

namespace eCAL
{
  CSyncMemoryFile::CSyncMemoryFile(const std::string& base_name_, size_t size_, SSyncMemoryFileAttr attr_) :
    m_attr(attr_),
    m_created(false)
  {
    Create(base_name_, size_);
  }

  CSyncMemoryFile::~CSyncMemoryFile()
  {
    Destroy();
  }

  bool CSyncMemoryFile::Connect(const std::string& process_id_)
  {
    if (!m_created) return false;

    // a local subscriber is registering with its process id
    //   we have to open the send update event and the acknowledge event
    //   we have ONE memory file per publisher and 1 or 2 events per memory file

    // the event names
    const std::string event_snd_name = m_memfile_name + "_" + process_id_;
    const std::string event_ack_name = m_memfile_name + "_" + process_id_ + "_ack";

    // check for existing process
    const std::lock_guard<std::mutex> lock(m_event_handle_map_sync);
    const EventHandleMapT::iterator iter = m_event_handle_map.find(process_id_);

    // add a new process id and create the sync and acknowledge event
    if (iter == m_event_handle_map.end())
    {
      SEventHandlePair event_pair;
      gOpenEvent(&event_pair.event_snd, event_snd_name);
      gOpenEvent(&event_pair.event_ack, event_ack_name);
      m_event_handle_map.insert(std::pair<std::string, SEventHandlePair>(process_id_, event_pair));
      return true;
    }
    else
    {
      // okay we have registered process events for that process id
      // we have to check the acknowledge event because it's possible that this
      // event was deactivated by a sync timeout in SendSyncEvents
      if (!gEventIsValid(iter->second.event_ack))
      {
        gOpenEvent(&iter->second.event_ack, event_ack_name);
      }
      return true;
    }
  }

  bool CSyncMemoryFile::Disconnect(const std::string& process_id_)
  {
    if (!m_created) return false;

    // a local subscriber connection timed out
    // we close the associated sync events and
    // remove them from the event handle map

    const std::lock_guard<std::mutex> lock(m_event_handle_map_sync);
    const EventHandleMapT::const_iterator iter = m_event_handle_map.find(process_id_);
    if (iter != m_event_handle_map.end())
    {
      const SEventHandlePair event_pair = iter->second;
      gCloseEvent(event_pair.event_snd);
      gCloseEvent(event_pair.event_ack);
      m_event_handle_map.erase(iter);
      return true;
    }

    return false;
  }

  bool CSyncMemoryFile::CheckSize(size_t size_)
  {
    if (!m_created) return false;

    // we recreate a memory file if the file size is too small
    const bool file_to_small = m_memfile.MaxDataSize() < (sizeof(SMemFileHeader) + size_);
    if (file_to_small)
    {
#ifndef NDEBUG
      Logging::Log(log_level_debug4, m_base_name + "::CSyncMemoryFile::CheckSize - RECREATE");
#endif
      // estimate size of memory file
      const size_t memfile_size = sizeof(SMemFileHeader) + size_ + static_cast<size_t>((static_cast<float>(m_attr.reserve) / 100.0f) * static_cast<float>(size_));

      // recreate the file
      if (!Recreate(memfile_size)) return false;

      // return true to trigger registration and immediately inform listening subscribers
      return true;
    }

    return false;
  }

  bool CSyncMemoryFile::Write(CPayloadWriter& payload_, const SWriterAttr& data_)
  {
    if (!m_created)
    {
      Logging::Log(log_level_error, m_base_name + "::CSyncMemoryFile::Write - FAILED (m_created == false)");
      return false;
    }

    // store acknowledge timeout parameter
    m_attr.timeout_ack_ms = data_.acknowledge_timeout_ms;
    if (m_attr.timeout_ack_ms < 0) m_attr.timeout_ack_ms = 0;

    // write header and payload into the memory file
#ifndef NDEBUG
    Logging::Log(log_level_debug4, m_base_name + "::CSyncMemoryFile::Write");
#endif

    // create user file header
    struct SMemFileHeader memfile_hdr;
    // set data size
    memfile_hdr.data_size         = static_cast<uint64_t>(data_.len);
    // set header id
    memfile_hdr.id                = static_cast<uint64_t>(data_.id);
    // set header clock
    memfile_hdr.clock             = static_cast<uint64_t>(data_.clock);
    // set header time
    memfile_hdr.time              = static_cast<int64_t>(data_.time);
    // set header hash
    memfile_hdr.hash              = static_cast<uint64_t>(data_.hash);
    // set zero copy
    memfile_hdr.options.zero_copy = static_cast<unsigned char>(data_.zero_copy);
    // set acknowledge timeout
    memfile_hdr.ack_timout_ms     = static_cast<int64_t>(data_.acknowledge_timeout_ms);

    // acquire write access
    bool write_access = m_memfile.GetWriteAccess(static_cast<int>(m_attr.timeout_open_ms));

    // maybe it's locked by a zombie or a crashed process
    // so we try to recreate a new one
    if (!write_access)
    {
#ifndef NDEBUG
      Logging::Log(log_level_debug2, m_base_name + "::CSyncMemoryFile::Write::GetWriteAccess - FAILED");
#endif

      // try to recreate the memory file
      if (!Recreate(m_memfile.MaxDataSize())) return false;

      // then try to get access again
      write_access = m_memfile.GetWriteAccess(static_cast<int>(m_attr.timeout_open_ms));
      // still no chance ? hell .... we give up
      if (!write_access)
      {
        Logging::Log(log_level_error, m_base_name + "::CSyncMemoryFile::Write::GetWriteAccess - FAILED FINALLY");
        return false;
      }
    }

    // now write content
    bool written(true);
    size_t wbytes(0);

    // write the user file header
    written &= m_memfile.WriteBuffer(&memfile_hdr, memfile_hdr.hdr_size, wbytes) > 0;
    wbytes += memfile_hdr.hdr_size;
    // write the buffer
    if (data_.len > 0)
    {
      written &= m_memfile.WritePayload(payload_, data_.len, wbytes) > 0;
    }
    // release write access
    m_memfile.ReleaseWriteAccess();

    // and fire the publish event for local subscriber
    if (written) SyncContent();

    if (written)
    {
#ifndef NDEBUG
      Logging::Log(log_level_debug4, m_base_name + "::CSyncMemoryFile::Write - SUCCESS : " + std::to_string(data_.len) + " Bytes written");
#endif
    }
    else
    {
      Logging::Log(log_level_error, m_base_name + "::CSyncMemoryFile::Write - FAILED (written == false)");
    }

    // return success
    return written;
  }

  std::string CSyncMemoryFile::GetName() const
  {
    return m_memfile_name;
  }

  size_t CSyncMemoryFile::GetSize() const
  {
    return m_attr.min_size;
  }

  bool CSyncMemoryFile::Create(const std::string& base_name_, size_t size_)
  {
    if (m_created) return false;

    // build unique memory file name
    m_base_name = base_name_;
    m_memfile_name = eCAL::memfile::BuildRandomMemFileName(base_name_);

    // create new memory file object
    // with additional space for SMemFileHeader
    size_t memfile_size = sizeof(SMemFileHeader) + size_;
    // check for minimal size
    if (memfile_size < m_attr.min_size) memfile_size = m_attr.min_size;

    // create the memory file
    if (!m_memfile.Create(m_memfile_name.c_str(), true, memfile_size))
    {
      Logging::Log(log_level_error, std::string(m_base_name + "::CSyncMemoryFile::Create - FAILED : ") + m_memfile_name);
      return false;
    }

#ifndef NDEBUG
    Logging::Log(log_level_debug2, std::string(m_base_name + "::CSyncMemoryFile::Create - SUCCESS : ") + m_memfile_name);
#endif

    // initialize memory file with empty header
    struct SMemFileHeader memfile_hdr;
    m_memfile.GetWriteAccess(static_cast<int>(m_attr.timeout_open_ms));
    m_memfile.WriteBuffer(&memfile_hdr, memfile_hdr.hdr_size, 0);
    m_memfile.ReleaseWriteAccess();

    // it's created
    m_created = true;

    return true;
  }

  bool CSyncMemoryFile::Destroy()
  {
    if (!m_created) return false;

    // state destruction in progress
    m_created = false;

    // reset memory file name
    m_memfile_name.clear();

    // disconnect all processes
    DisconnectAll();

    // destroy the file
    if (!m_memfile.Destroy(true))
    {
#ifndef NDEBUG
      Logging::Log(log_level_debug2, std::string(m_base_name + "::CSyncMemoryFile::Destroy - FAILED : ") + m_memfile.Name());
#endif
      return false;
    }

#ifndef NDEBUG
    Logging::Log(log_level_debug2, std::string(m_base_name + "::CSyncMemoryFile::Destroy - SUCCESS : ") + m_memfile.Name());
#endif
    return true;
  }

  bool CSyncMemoryFile::Recreate(size_t size_)
  {
    // collect id's of the currently connected processes
    std::vector<std::string> process_id_list;
    {
      const std::lock_guard<std::mutex> lock(m_event_handle_map_sync);
      for (const auto& event_handle : m_event_handle_map)
      {
        process_id_list.push_back(event_handle.first);
      }
    }

    // destroy existing memory file object
    Destroy();

    // create a new one
    if (!Create(m_base_name, size_)) return false;

    // reconnect processes
    for (const auto& process_id : process_id_list)
    {
      Connect(process_id);
    }

    return true;
  }

  void CSyncMemoryFile::SyncContent()
  {
    if (!m_created) return;

    // fire the publisher events
    // connected subscribers will read the content from the memory file

    const std::lock_guard<std::mutex> lock(m_event_handle_map_sync);

    // "eat" old acknowledge events :)
    if (m_attr.timeout_ack_ms != 0)
    {
      for (const auto& event_handle : m_event_handle_map)
      {
        while (gWaitForEvent(event_handle.second.event_ack, 0)) {}
      }
    }

    // send sync (memory file update) event
    for (const auto& event_handle : m_event_handle_map)
    {
      // send sync event
      gSetEvent(event_handle.second.event_snd);
    }

    // wait for acknowledgment event from receiver side
    if (m_attr.timeout_ack_ms != 0)
    {
      // take start time for all acknowledge timeouts
      const auto start_time = std::chrono::steady_clock::now();

      for (auto& event_handle : m_event_handle_map)
      {
        const auto time_since_start = std::chrono::steady_clock::now() - start_time;
        const auto time_to_wait     = std::chrono::milliseconds(m_attr.timeout_ack_ms)- time_since_start;
        long       time_to_wait_ms  = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(time_to_wait).count());
        if (time_to_wait_ms <= 0) time_to_wait_ms = 0;

        if (!gWaitForEvent(event_handle.second.event_ack, time_to_wait_ms))
        {
          // we close the event immediately to not waste time in the next
          // write call, the event will be reopened later
          // in ApplyLocSubscription if the connection still exists
          gCloseEvent(event_handle.second.event_ack);
          // invalidate it
          gInvalidateEvent(&event_handle.second.event_ack);
#ifndef NDEBUG
          Logging::Log(log_level_debug2, m_base_name + "::CSyncMemoryFile::SignalWritten - ACK event timeout");
#endif
        }
      }
    }

#ifndef NDEBUG
    Logging::Log(log_level_debug4, m_base_name + "::CSyncMemoryFile::SignalWritten");
#endif
  }

  void CSyncMemoryFile::DisconnectAll()
  {
    const std::lock_guard<std::mutex> lock(m_event_handle_map_sync);

    // fire acknowledge events, to unlock blocking send function
    for (const auto& event_handle : m_event_handle_map)
    {
      gSetEvent(event_handle.second.event_ack);
    }

    // close all events
    for (const auto& event_handle : m_event_handle_map)
    {
      gCloseEvent(event_handle.second.event_snd);
      gCloseEvent(event_handle.second.event_ack);
    }

    // invalidate all events
    for (auto& event_handle : m_event_handle_map)
    {
      gInvalidateEvent(&event_handle.second.event_snd);
      gInvalidateEvent(&event_handle.second.event_ack);
    }

    // clear event map
    m_event_handle_map.clear();
  }
}
