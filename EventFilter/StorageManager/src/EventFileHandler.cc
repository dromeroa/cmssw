// $Id: EventFileHandler.cc,v 1.5 2009/08/28 16:41:26 mommsen Exp $
/// @file: EventFileHandler.cc

#include <EventFilter/StorageManager/interface/EventFileHandler.h>
#include <EventFilter/StorageManager/interface/I2OChain.h>
#include <IOPool/Streamer/interface/EventMessage.h>

#include <iostream>
 
using namespace stor;


EventFileHandler::EventFileHandler
(
  InitMsgSharedPtr view,
  FilesMonitorCollection::FileRecordPtr fileRecord,
  const DiskWritingParams& dwParams,
  const long long& maxFileSize
) :
FileHandler(fileRecord, dwParams, maxFileSize),
_writer(
  fileRecord->completeFileName()+".dat",
  fileRecord->completeFileName()+".ind"
)
{
  writeHeader(view);
}


EventFileHandler::~EventFileHandler()
{
  closeFile();
}


void EventFileHandler::writeHeader(InitMsgSharedPtr view)
{
  InitMsgView initView(&(*view)[0]);
  _writer.doOutputHeader(initView);
  _fileRecord->fileSize += view->size();
  _lastEntry = utils::getCurrentTime();
}


void EventFileHandler::writeEvent(const I2OChain& event)
{
  edm::StreamerFileWriterEventParams evtParams;

  event.hltTriggerBits(evtParams.hltBits);
  evtParams.headerPtr = (char*) event.headerLocation();
  evtParams.headerSize = event.headerSize();

  unsigned int fragCount = event.fragmentCount();
  evtParams.fragmentCount = fragCount;

  for (unsigned int idx = 0; idx < fragCount; ++idx)
    {
      evtParams.fragmentIndex = idx;
      evtParams.dataPtr = (char*) event.dataLocation(idx);
      evtParams.dataSize = event.dataSize(idx);

      _writer.doOutputEventFragment(evtParams);
    }

  _fileRecord->fileSize += event.totalDataSize();
  ++_fileRecord->eventCount;
  _lastEntry = utils::getCurrentTime();
}


bool EventFileHandler::tooOld(const utils::time_point_t currentTime)
{
  if (_diskWritingParams._lumiSectionTimeOut > 0 && 
    (currentTime - _lastEntry) > _diskWritingParams._lumiSectionTimeOut)
  {
    _closingReason = FilesMonitorCollection::FileRecord::timeout;
    return true;
  }
  else
  {
    return false;
  }
}


bool EventFileHandler::isFromLumiSection(const uint32_t lumiSection)
{
  if (lumiSection == _fileRecord->lumiSection)
  {
    _closingReason = FilesMonitorCollection::FileRecord::endOfLS;
    return true;
  }
  else
  {
    return false;
  }
}


void EventFileHandler::closeFile()
{
  _writer.stop();
  _fileRecord->fileSize += _writer.getStreamEOFSize();
  setAdler(_writer.get_adler32_stream(), _writer.get_adler32_index());
  moveFileToClosed(true);
  writeToSummaryCatalog();
  updateDatabase();
}



/// emacs configuration
/// Local Variables: -
/// mode: c++ -
/// c-basic-offset: 2 -
/// indent-tabs-mode: nil -
/// End: -
