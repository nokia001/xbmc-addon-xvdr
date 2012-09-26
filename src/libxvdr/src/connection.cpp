/*
 *      Copyright (C) 2010 Alwin Esch (Team XBMC)
 *      Copyright (C) 2011 Alexander Pipelka
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdlib.h>
#include <string.h>

#include "xvdr/connection.h"
#include "xvdr/callbacks.h"
#include "xvdr/responsepacket.h"
#include "xvdr/requestpacket.h"
#include "xvdr/command.h"

extern "C" {
#include "libTcpSocket/os-dependent_socket.h"
}

using namespace XVDR;

#define CMD_LOCK MutexLock CmdLock((Mutex*)&m_Mutex)
#define SEEK_POSSIBLE 0x10 // flag used to check if protocol allows seeks

Connection::Connection()
 : m_statusinterface(false)
 , m_aborting(false)
 , m_timercount(0)
 , m_updatechannels(2)
{
}

Connection::~Connection()
{
  Abort();
  Cancel(1);
  Close();
}

bool Connection::Open(const std::string& hostname, const char* name)
{
  m_aborting = false;

  if(!Session::Open(hostname, name))
    return false;

  if(name != NULL) {
    SetDescription(name);
  }
  return true;
}

bool Connection::Login()
{
  if(!Session::Login())
    return false;

  Start();
  return true;
}

void Connection::Abort()
{
  CMD_LOCK;
  m_aborting = true;
  Session::Abort();
}

void Connection::SignalConnectionLost()
{
  CMD_LOCK;

  if(m_aborting)
    return;

  Session::SignalConnectionLost();
}

void Connection::OnDisconnect()
{
  XVDRNotification(XVDR_ERROR, XVDRGetLocalizedString(30044));
}

void Connection::OnReconnect()
{
  XVDRNotification(XVDR_INFO, XVDRGetLocalizedString(30045));

  EnableStatusInterface(m_statusinterface, true);
  ChannelFilter(m_ftachannels, m_nativelang, m_caids, true);
  SetUpdateChannels(m_updatechannels, true);

  XVDRTriggerTimerUpdate();
  XVDRTriggerRecordingUpdate();
}

ResponsePacket* Connection::ReadResult(RequestPacket* vrp)
{
  m_Mutex.Lock();

  SMessage &message(m_queue[vrp->getSerial()]);
  message.event = new CondWait();
  message.pkt   = NULL;

  m_Mutex.Unlock();

  if(!Session::SendMessage(vrp))
  {
    CMD_LOCK;
    m_queue.erase(vrp->getSerial());
    return NULL;
  }

  message.event->Wait(m_timeout);

  m_Mutex.Lock();

  ResponsePacket* vresp = message.pkt;
  delete message.event;

  m_queue.erase(vrp->getSerial());

  m_Mutex.Unlock();

  return vresp;
}

bool Connection::GetDriveSpace(long long *total, long long *used)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_RECORDINGS_DISKSIZE))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  uint32_t totalspace    = vresp->extract_U32();
  uint32_t freespace     = vresp->extract_U32();
  /* vresp->extract_U32(); percent not used */

  *total = totalspace;
  *used  = (totalspace - freespace);

  /* Convert from kBytes to Bytes */
  *total *= 1024;
  *used  *= 1024;

  delete vresp;
  return true;
}

bool Connection::SupportChannelScan()
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_SCAN_SUPPORTED))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  uint32_t ret = vresp->extract_U32();
  delete vresp;
  return ret == XVDR_RET_OK ? true : false;
}

bool Connection::EnableStatusInterface(bool onOff, bool direct)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_ENABLESTATUSINTERFACE)) return false;
  if (!vrp.add_U8(onOff)) return false;

  ResponsePacket* vresp = direct ? Session::ReadResult(&vrp) : ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  uint32_t ret = vresp->extract_U32();
  delete vresp;
  if(ret == XVDR_RET_OK)
  {
    m_statusinterface = onOff;
    return true;
  }

  return false;
}

bool Connection::SetUpdateChannels(uint8_t method, bool direct)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_UPDATECHANNELS)) return false;
  if (!vrp.add_U8(method)) return false;

  ResponsePacket* vresp = direct ? Session::ReadResult(&vrp) : ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_INFO, "Setting channel update method not supported by server. Consider updating the XVDR server.");
    return false;
  }

  XVDRLog(XVDR_INFO, "Channel update method set to %i", method);

  uint32_t ret = vresp->extract_U32();
  delete vresp;
  if (ret == XVDR_RET_OK)
  {
    m_updatechannels = method;
    return true;
  }

  return false;
}

bool Connection::ChannelFilter(bool fta, bool nativelangonly, std::vector<int>& caids, bool direct)
{
  std::size_t count = caids.size();
  RequestPacket vrp;

  if (!vrp.init(XVDR_CHANNELFILTER)) return false;
  if (!vrp.add_U32(fta)) return false;
  if (!vrp.add_U32(nativelangonly)) return false;
  if (!vrp.add_U32(count)) return false;

  for(int i = 0; i < count; i++)
    if (!vrp.add_U32(caids[i])) return false;

  ResponsePacket* vresp = direct ? Session::ReadResult(&vrp) : ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_INFO, "Channel filter method not supported by server. Consider updating the XVDR server.");
    return false;
  }

  XVDRLog(XVDR_INFO, "Channel filter set");

  uint32_t ret = vresp->extract_U32();
  delete vresp;

  if (ret == XVDR_RET_OK)
  {
    m_ftachannels = fta;
    m_nativelang = nativelangonly;
    m_caids = caids;
    return true;
  }

  return false;
}

int Connection::GetChannelsCount()
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_CHANNELS_GETCOUNT))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return -1;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return -1;
  }

  uint32_t count = vresp->extract_U32();

  delete vresp;
  return count;
}

bool Connection::GetChannelsList(bool radio)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_CHANNELS_GETCHANNELS))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }
  if (!vrp.add_U32(radio))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't add parameter to RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  while (!vresp->end())
  {
	Channel tag;

    tag[channel_number] = vresp->extract_U32();
    tag[channel_name] = vresp->extract_String();
    tag[channel_uid] = vresp->extract_U32();
                            vresp->extract_U32(); // still here for compatibility
    tag[channel_encryptionsystem] = vresp->extract_U32();
                            vresp->extract_U32(); // uint32_t vtype - currently unused
    tag[channel_isradio] = radio;
    tag[channel_inputformat] = "";
    tag[channel_streamurl] = "";
    tag[channel_iconpath] = "";
    tag[channel_ishidden] = false;

    XVDRTransferChannelEntry(tag);
  }

  delete vresp;
  return true;
}

bool Connection::GetEPGForChannel(uint32_t channeluid, time_t start, time_t end)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_EPG_GETFORCHANNEL))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  if (!vrp.add_U32(channeluid) || !vrp.add_U32(start) || !vrp.add_U32(end - start))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't add parameter to RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  if (!vresp->serverError())
  {
    while (!vresp->end())
    {
      Epg tag;

      tag[epg_uid] = channeluid;
      tag[epg_broadcastid] = vresp->extract_U32();
      uint32_t starttime = vresp->extract_U32();
      tag[epg_starttime] = starttime;
      tag[epg_endtime] = starttime + vresp->extract_U32();
      uint32_t content        = vresp->extract_U32();
      tag[epg_genretype] = content & 0xF0;
      tag[epg_genresubtype] = content & 0x0F;
      //tag[epg_genredescription] = "";
      tag[epg_parentalrating] = vresp->extract_U32();
      tag[epg_title] = vresp->extract_String();
      tag[epg_plotoutline] = vresp->extract_String();
      tag[epg_plot] = vresp->extract_String();

      XVDRTransferEpgEntry(tag);
    }
  }

  delete vresp;
  return true;
}


/** OPCODE's 60 - 69: XVDR network functions for timer access */

int Connection::GetTimersCount()
{
  // return caches values on connection loss
  if(ConnectionLost())
    return m_timercount;

  RequestPacket vrp;
  if (!vrp.init(XVDR_TIMER_GETCOUNT))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return -1;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return m_timercount;
  }

  m_timercount = vresp->extract_U32();

  delete vresp;
  return m_timercount;
}

void Connection::ReadTimerPacket(ResponsePacket* resp, Timer& tag) {
  tag[timer_index] = resp->extract_U32();

  int iActive           = resp->extract_U32();
  int iRecording        = resp->extract_U32();
  int iPending          = resp->extract_U32();

  tag[timer_state] = iRecording;
  /*else if (iPending || iActive)
    tag.state = PVR_TIMER_STATE_SCHEDULED;
  else
    tag.state = PVR_TIMER_STATE_CANCELLED;*/
  tag[timer_priority] = resp->extract_U32();
  tag[timer_lifetime] = resp->extract_U32();
                          resp->extract_U32(); // channel number - unused
  tag[timer_channeluid] =  resp->extract_U32();
  tag[timer_starttime] = resp->extract_U32();
  tag[timer_endtime] = resp->extract_U32();
  tag[timer_firstday] = resp->extract_U32();
  int weekdays = resp->extract_U32();
  tag[timer_weekdays] = weekdays;
  tag[timer_isrepeating] = ((weekdays == 0) ? false : true);

  const char* title = resp->extract_String();
  tag[timer_marginstart] = 0;
  tag[timer_marginend] = 0;

  char* p = (char*)strrchr(title, '~');
  if(p == NULL || *p == 0) {
	  tag[timer_title] = title;
	  tag[timer_directory] =  "";
  }
  else {
	  const char* name = p + 1;

	  p[0] = 0;
	  const char* dir = title;

	  // replace dir separators
	  for(p = (char*)dir; *p != 0; p++)
		  if(*p == '~') *p = '/';

	  tag[timer_title] = name;
	  tag[timer_directory] =  dir;
  }
}

bool Connection::GetTimerInfo(unsigned int timernumber, Timer& tag)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_TIMER_GET))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  if (!vrp.add_U32(timernumber))
    return false;

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    delete vresp;
    return false;
  }

  uint32_t returnCode = vresp->extract_U32();
  if (returnCode != XVDR_RET_OK)
  {
    delete vresp;
    if (returnCode == XVDR_RET_DATAUNKNOWN)
      return false;
    else if (returnCode == XVDR_RET_ERROR)
      return false;
  }

  ReadTimerPacket(vresp, tag);

  delete vresp;
  return true;
}

bool Connection::GetTimersList()
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_TIMER_GETLIST))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    delete vresp;
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  uint32_t numTimers = vresp->extract_U32();
  if (numTimers > 0)
  {
    while (!vresp->end())
    {
      Timer timer;
      ReadTimerPacket(vresp, timer);
      XVDRTransferTimerEntry(timer);
    }
  }
  delete vresp;
  return true;
}

bool Connection::AddTimer(const Timer& timer)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_TIMER_ADD))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  // add directory in front of the title
  std::string path;
  std::string directory = timer[timer_directory];
  std::string title = timer[timer_title];

  if(!directory.empty()) {
    path += directory;
    if(path == "/") {
      path.clear();
    }
    else if(path.size() > 1) {
      if(path[0] == '/') {
        path = path.substr(1);
      }
    }

    if(path.size() > 0 && path[path.size()-1] != '/') {
      path += "/";
    }
  }

  if(!title.empty()) {
    path += title;
  }

  // replace directory separators
  for(std::size_t i=0; i<path.size(); i++) {
    if(path[i] == '/' || path[i] == '\\') {
      path[i] = '~';
    }
  }

  if(path.empty()) {
    XVDRLog(XVDR_ERROR, "%s - Empty filename !", __FUNCTION__);
    return false;
  }

  // use timer margin to calculate start/end times
  uint32_t starttime = (uint32_t)timer[timer_starttime] - (uint32_t)timer[timer_marginstart] * 60;
  uint32_t endtime = (uint32_t)timer[timer_endtime] + (uint32_t)timer[timer_marginend] * 60;

  vrp.add_U32(1);
  vrp.add_U32(timer[timer_priority]);
  vrp.add_U32(timer[timer_lifetime]);
  vrp.add_U32(timer[timer_channeluid]);
  vrp.add_U32(starttime);
  vrp.add_U32(endtime);
  vrp.add_U32(timer[timer_isrepeating] ? timer[timer_firstday] : 0);
  vrp.add_U32(timer[timer_weekdays]);
  vrp.add_String(path.c_str());
  vrp.add_String("");

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }
  uint32_t returnCode = vresp->extract_U32();
  delete vresp;

  return (returnCode == XVDR_RET_OK);
}

bool Connection::DeleteTimer(uint32_t timerindex, bool force)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_TIMER_DELETE))
    return false;

  if (!vrp.add_U32(timerindex))
    return false;

  if (!vrp.add_U32(force))
    return false;

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return false;
  }

  uint32_t returnCode = vresp->extract_U32();
  delete vresp;

  return (returnCode == XVDR_RET_OK);
}

bool Connection::UpdateTimer(const Timer& timer)
{
  // use timer margin to calculate start/end times
  uint32_t starttime = timer[timer_starttime] - timer[timer_marginstart] * 60;
  uint32_t endtime = timer[timer_endtime] + timer[timer_marginend] * 60;

  std::string dir = timer[timer_directory];
  while(dir[dir.size()-1] == '/' && dir.size() > 1)
    dir = dir.substr(0, dir.size()-1);

  std::string name = timer[timer_title];
  std::string title;

  if(!dir.empty() && dir != "/")
	  title = dir + "/";

  title += name;

  // replace dir separators
  for(std::string::iterator i = title.begin(); i != title.end(); i++)
	  if(*i == '/') *i = '~';

  RequestPacket vrp;
  vrp.init(XVDR_TIMER_UPDATE);
  vrp.add_U32(timer[timer_index]);
  vrp.add_U32(2);
  vrp.add_U32(timer[timer_priority]);
  vrp.add_U32(timer[timer_lifetime]);
  vrp.add_U32(timer[timer_channeluid]);
  vrp.add_U32(starttime);
  vrp.add_U32(endtime);
  vrp.add_U32(timer[timer_isrepeating] ? timer[timer_firstday] : 0);
  vrp.add_U32(timer[timer_weekdays]);
  vrp.add_String(title.c_str());
  vrp.add_String("");

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return false;
  }

  uint32_t returnCode = vresp->extract_U32();
  delete vresp;

  return (returnCode == XVDR_RET_OK);
}

int Connection::GetRecordingsCount()
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_RECORDINGS_GETCOUNT))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return -1;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return -1;
  }

  uint32_t count = vresp->extract_U32();

  delete vresp;
  return count;
}

bool Connection::GetRecordingsList()
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_RECORDINGS_GETLIST))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
  {
    XVDRLog(XVDR_ERROR, "%s - Can't get response packet", __FUNCTION__);
    return false;
  }

  while (!vresp->end())
  {
	RecordingEntry rec;
	rec[recording_time] = vresp->extract_U32();
	rec[recording_duration] = vresp->extract_U32();
	rec[recording_priority] = vresp->extract_U32();
	rec[recording_lifetime] = vresp->extract_U32();
	rec[recording_channelname] = vresp->extract_String();
	rec[recording_title] = vresp->extract_String();
	rec[recording_plotoutline] = vresp->extract_String();
	rec[recording_plot] = vresp->extract_String();
	rec[recording_directory] = vresp->extract_String();
	rec[recording_id] = vresp->extract_String();
	rec[recording_streamurl] = "";
	rec[recording_genretype] = 0;
	rec[recording_genresubtype] = 0;
	rec[recording_playcount] = 0;

    XVDRTransferRecordingEntry(rec);
  }

  delete vresp;

  return true;
}

bool Connection::RenameRecording(const std::string& recid, const std::string& newname)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_RECORDINGS_RENAME))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  // add uid
  XVDRLog(XVDR_DEBUG, "%s - uid: %s", __FUNCTION__, recid.c_str());

  vrp.add_String(recid.c_str());
  vrp.add_String(newname.c_str());

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return false;
  }

  uint32_t returnCode = vresp->extract_U32();
  delete vresp;

  return (returnCode == XVDR_RET_OK);
}

bool Connection::DeleteRecording(const std::string& recid)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_RECORDINGS_DELETE))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  vrp.add_String(recid.c_str());

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return false;
  }

  uint32_t returnCode = vresp->extract_U32();
  delete vresp;

  return (returnCode == XVDR_RET_OK);
}

bool Connection::OnResponsePacket(ResponsePacket* pkt)
{
  return false;
}

bool Connection::SendPing()
{
  XVDRLog(XVDR_DEBUG, "%s", __FUNCTION__);

  RequestPacket vrp;
  if (!vrp.init(XVDR_PING))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  ResponsePacket* vresp = Session::ReadResult(&vrp);
  delete vresp;

  return (vresp != NULL);
}

void Connection::Action()
{
  uint32_t lastPing = 0;
  ResponsePacket* vresp;

  SetPriority(19);

  while (Running())
  {
    // try to reconnect
    if(ConnectionLost() && !TryReconnect())
    {
      SleepMs(1000);
      continue;
   }

    // read message
    vresp = Session::ReadMessage();

    // check if the connection is still up
    if ((vresp == NULL) && (time(NULL) - lastPing) > 5)
    {
      CMD_LOCK;
      if(m_queue.empty())
      {
        lastPing = time(NULL);

        if(!SendPing())
          SignalConnectionLost();
      }
    }

    // there wasn't any response
    if (vresp == NULL)
      continue;

    // CHANNEL_REQUEST_RESPONSE

    if (vresp->getChannelID() == XVDR_CHANNEL_REQUEST_RESPONSE)
    {
      CMD_LOCK;
      SMessages::iterator it = m_queue.find(vresp->getRequestID());
      if (it != m_queue.end())
      {
        it->second.pkt = vresp;
        it->second.event->Signal();
      }
      else
      {
        delete vresp;
      }
    }

    // CHANNEL_STATUS

    else if (vresp->getChannelID() == XVDR_CHANNEL_STATUS)
    {
      if (vresp->getRequestID() == XVDR_STATUS_MESSAGE)
      {
        uint32_t type = vresp->extract_U32();
        const char* msgstr = vresp->extract_String();

        if (type == 2)
          XVDRNotification(XVDR_ERROR, msgstr);
        if (type == 1)
          XVDRNotification(XVDR_WARNING, msgstr);
        else
          XVDRNotification(XVDR_INFO, msgstr);

      }
      else if (vresp->getRequestID() == XVDR_STATUS_RECORDING)
      {
                           vresp->extract_U32(); // device currently unused
        uint32_t on      = vresp->extract_U32();
        const char* str1 = vresp->extract_String();
        const char* str2 = vresp->extract_String();

        XVDRRecording(str1, str2, on);
        XVDRTriggerTimerUpdate();
      }
      else if (vresp->getRequestID() == XVDR_STATUS_TIMERCHANGE)
      {
        XVDRLog(XVDR_DEBUG, "Server requested timer update");
        XVDRTriggerTimerUpdate();
      }
      else if (vresp->getRequestID() == XVDR_STATUS_CHANNELCHANGE)
      {
        XVDRLog(XVDR_DEBUG, "Server requested channel update");
        XVDRTriggerChannelUpdate();
      }
      else if (vresp->getRequestID() == XVDR_STATUS_RECORDINGSCHANGE)
      {
        XVDRLog(XVDR_DEBUG, "Server requested recordings update");
        XVDRTriggerRecordingUpdate();
      }

      delete vresp;
    }

    // UNKOWN CHANNELID

    else if (!OnResponsePacket(vresp))
    {
      XVDRLog(XVDR_ERROR, "%s - Rxd a response packet on channel %lu !!", __FUNCTION__, vresp->getChannelID());
      delete vresp;
    }
  }
}

int Connection::GetChannelGroupCount(bool automatic)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_CHANNELGROUP_GETCOUNT))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return 0;
  }

  if (!vrp.add_U32(automatic))
  {
    return 0;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return 0;
  }

  uint32_t count = vresp->extract_U32();

  delete vresp;
  return count;
}

bool Connection::GetChannelGroupList(bool bRadio)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_CHANNELGROUP_LIST))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  vrp.add_U8(bRadio);

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return false;
  }

  while (!vresp->end())
  {
    ChannelGroup group;

    group[channelgroup_name] = vresp->extract_String();
    group[channelgroup_isradio] = vresp->extract_U8();
    XVDRTransferChannelGroup(group);
  }

  delete vresp;
  return true;
}

bool Connection::GetChannelGroupMembers(const std::string& groupname, bool radio)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_CHANNELGROUP_MEMBERS))
  {
    XVDRLog(XVDR_ERROR, "%s - Can't init RequestPacket", __FUNCTION__);
    return false;
  }

  vrp.add_String(groupname.c_str());
  vrp.add_U8(radio);

  ResponsePacket* vresp = ReadResult(&vrp);
  if (vresp == NULL || vresp->noResponse())
  {
    delete vresp;
    return false;
  }

  while (!vresp->end())
  {
    ChannelGroupMember member;
    member[channelgroupmember_name] = groupname;
    member[channelgroupmember_uid] = vresp->extract_U32();
    member[channelgroupmember_number] = vresp->extract_U32();

    XVDRTransferChannelGroupMember(member);
  }

  delete vresp;
  return true;
}

bool Connection::OpenRecording(const std::string& recid)
{
  RequestPacket vrp;
  if (!vrp.init(XVDR_RECSTREAM_OPEN) ||
      !vrp.add_String(recid.c_str()))
  {
    return false;
  }

  ResponsePacket* vresp = ReadResult(&vrp);
  if (!vresp)
    return false;

  uint32_t returnCode = vresp->extract_U32();
  if (returnCode == XVDR_RET_OK)
  {
    m_currentPlayingRecordFrames    = vresp->extract_U32();
    m_currentPlayingRecordBytes     = vresp->extract_U64();
    m_currentPlayingRecordPosition  = 0;
    m_recid = recid;
  }
  else {
    XVDRLog(XVDR_ERROR, "%s - Can't open recording", __FUNCTION__);
    m_recid.clear();

  }
  delete vresp;
  return (returnCode == XVDR_RET_OK);
}

bool Connection::CloseRecording()
{
  if(m_recid.empty())
    return false;

  RequestPacket vrp;
  vrp.init(XVDR_RECSTREAM_CLOSE);
  m_recid.clear();

  return ReadSuccess(&vrp);
}

int Connection::ReadRecording(unsigned char* buf, uint32_t buf_size)
{
  if (ConnectionLost() && !TryReconnect())
  {
    *buf = 0;
    SleepMs(100);
    return 1;
  }

  if (m_currentPlayingRecordPosition >= m_currentPlayingRecordBytes)
    return 0;

  ResponsePacket* vresp = NULL;

  RequestPacket vrp1;
  if (vrp1.init(XVDR_RECSTREAM_UPDATE) && ((vresp = ReadResult(&vrp1)) != NULL))
  {
    uint32_t frames = vresp->extract_U32();
    uint64_t bytes  = vresp->extract_U64();

    if(frames != m_currentPlayingRecordFrames || bytes != m_currentPlayingRecordBytes)
    {
      m_currentPlayingRecordFrames = frames;
      m_currentPlayingRecordBytes  = bytes;
      XVDRLog(XVDR_DEBUG, "Size of recording changed: %lu bytes (%u frames)", bytes, frames);
    }
    delete vresp;
  }

  RequestPacket vrp2;
  if (!vrp2.init(XVDR_RECSTREAM_GETBLOCK) ||
      !vrp2.add_U64(m_currentPlayingRecordPosition) ||
      !vrp2.add_U32(buf_size))
  {
    return 0;
  }

  vresp = ReadResult(&vrp2);
  if (!vresp)
    return -1;

  uint32_t length = vresp->getUserDataLength();
  uint8_t *data   = vresp->getUserData();
  if (length > buf_size)
  {
    XVDRLog(XVDR_ERROR, "%s: PANIC - Received more bytes as requested", __FUNCTION__);
    free(data);
    delete vresp;
    return 0;
  }

  memcpy(buf, data, length);
  m_currentPlayingRecordPosition += length;
  free(data);
  delete vresp;
  return length;
}

long long Connection::SeekRecording(long long pos, uint32_t whence)
{
  uint64_t nextPos = m_currentPlayingRecordPosition;

  switch (whence)
  {
    case SEEK_SET:
      nextPos = pos;
      break;

    case SEEK_CUR:
      nextPos += pos;
      break;

    case SEEK_END:
      nextPos = m_currentPlayingRecordBytes + pos;
      break;

    case SEEK_POSSIBLE:
      return 1;

    default:
      return -1;
  }

  if (nextPos > m_currentPlayingRecordBytes)
    return -1;

  m_currentPlayingRecordPosition = nextPos;

  return m_currentPlayingRecordPosition;
}

long long Connection::RecordingPosition(void)
{
  return m_currentPlayingRecordPosition;
}

long long Connection::RecordingLength(void)
{
  return m_currentPlayingRecordBytes;
}
