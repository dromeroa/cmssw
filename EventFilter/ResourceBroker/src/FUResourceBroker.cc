///////////////////////////////////////////////////////////////////////////////
//
// FUResourceBroker
// ----------------
//
//            10/20/2006 Philipp Schieferdecker <philipp.schieferdecker@cern.ch>
////////////////////////////////////////////////////////////////////////////////


#include "EventFilter/ResourceBroker/interface/FUResourceBroker.h"
#include "EventFilter/ResourceBroker/interface/BUProxy.h"

#include "EventFilter/Utilities/interface/RunBase.h"
#include "EventFilter/Utilities/interface/Crc.h"

#include "i2o/include/i2o/Method.h"

#include "interface/shared/include/i2oXFunctionCodes.h"

#include "xcept/include/xcept/tools.h"

#include "toolbox/include/toolbox/mem/HeapAllocator.h"
#include "toolbox/include/toolbox/mem/Reference.h"
#include "toolbox/include/toolbox/mem/MemoryPoolFactory.h"
#include "toolbox/include/toolbox/mem/exception/Exception.h"

#include "xoap/include/xoap/SOAPEnvelope.h"
#include "xoap/include/xoap/SOAPBody.h"
#include "xoap/include/xoap/domutils.h"

#include <iostream>
#include <sstream>


using namespace std;
using namespace evf;


////////////////////////////////////////////////////////////////////////////////
// construction/destruction
////////////////////////////////////////////////////////////////////////////////

//______________________________________________________________________________
FUResourceBroker::FUResourceBroker(xdaq::ApplicationStub *s)
  : xdaq::Application(s)
  , FEDProvider()
  , lock_(BSem::FULL)
  , gui_(0)
  , log_(getApplicationLogger())
  , fsm_(0)
  , i2oPool_(0)
  , resourceTable_(0)
  , instance_(0)
  , runNumber_(0)
  , nbShmClients_(0)
  , nbMBTot_(0.0)
  , nbMBPerSec_(0.0)
  , nbMBPerSecMin_(0.0)
  , nbMBPerSecMax_(0.0)
  , nbMBPerSecAvg_(0.0)
  , nbEvents_(0)
  , nbEventsPerSec_(0)
  , nbEventsPerSecMin_(0)
  , nbEventsPerSecMax_(0)
  , nbEventsPerSecAvg_(0)
  , nbAllocatedEvents_(0)
  , nbPendingRequests_(0)
  , nbReceivedEvents_(0)
  , nbProcessedEvents_(0)
  , nbLostEvents_(0)
  , nbDataErrors_(0)
  , nbCrcErrors_(0)
  , shmMode_(false)
    , eventBufferSize_(4194304) // 4MB
  //, doDumpFragments_(false)
  , doDropEvents_(false)
  , doCrcCheck_(1)
  , buClassName_("BU")
  , buInstance_(0)
  , queueSize_(16)
  , nbAllocateSent_(0)
  , nbTakeReceived_(0)
  , nbTimeExpired_(0)
  , nbMeasurements_(0)
  , nbEventsLast_(0)
{
  // set source id in evf::RunBase
  url_     =
    getApplicationDescriptor()->getContextDescriptor()->getURL()+"/"+
    getApplicationDescriptor()->getURN();
  class_   =getApplicationDescriptor()->getClassName();
  instance_=getApplicationDescriptor()->getInstance();
  
  sourceId_=class_.toString()+"_"+instance_.toString();
  RunBase::sourceId_=sourceId_;
  
  // initialize the finite state machine
  fsm_=new EPStateMachine(log_);
  fsm_->init<FUResourceBroker>(this);

  // i2o callback for FU_TAKE messages from builder unit
  i2o::bind(this,
	    &FUResourceBroker::I2O_FU_TAKE_Callback,
	    I2O_FU_TAKE,
	    XDAQ_ORGANIZATION_ID);
  
  // bind HyperDAQ web pages
  xgi::bind(this,&evf::FUResourceBroker::webPageRequest,"Default");
  gui_=new WebGUI(this,fsm_);
  vector<toolbox::lang::Method*> methods=gui_->getMethods();
  vector<toolbox::lang::Method*>::iterator it;
  for (it=methods.begin();it!=methods.end();++it) {
    if ((*it)->type()=="cgi") {
      string name=static_cast<xgi::MethodSignature*>(*it)->name();
      xgi::bind(this,&evf::FUResourceBroker::webPageRequest,name);
    }
  }
  
  // allocate i2o memery pool
  string i2oPoolName=sourceId_+"_i2oPool";
  try {
    toolbox::mem::HeapAllocator *allocator=new toolbox::mem::HeapAllocator();
    toolbox::net::URN urn("toolbox-mem-pool",i2oPoolName);
    toolbox::mem::MemoryPoolFactory* poolFactory=
      toolbox::mem::getMemoryPoolFactory();
    i2oPool_=poolFactory->createPool(urn,allocator);
  }
  catch (toolbox::mem::exception::Exception& e) {
    string s="Failed to create pool: "+i2oPoolName;
    LOG4CPLUS_FATAL(log_,s);
    XCEPT_RETHROW(xcept::Exception,s,e);
  }
  
  // publish all parameters to app info space
  exportParameters();
}


//______________________________________________________________________________
FUResourceBroker::~FUResourceBroker()
{
  delete resourceTable_;
}



////////////////////////////////////////////////////////////////////////////////
// implementation of member functions
////////////////////////////////////////////////////////////////////////////////

//______________________________________________________________________________
FEDRawDataCollection* FUResourceBroker::rqstEvent(UInt_t& evtNumber,
						  UInt_t& buResId)
{
  if (!shmMode_) nbPendingRequests_.value_--;
  
  FEDRawDataCollection* result=resourceTable_->requestResource(evtNumber,buResId);

  lock_.take();
  if (itsTimeToAllocate()) sendAllocate();
  lock_.give();
  
  return result;
}


//______________________________________________________________________________
void FUResourceBroker::timeExpired(toolbox::task::TimerEvent& e)
{
  lock_.take();

  gui_->lockInfoSpaces();

  nbMeasurements_++;
 
  // number of events per second measurement
  nbEvents_      =resourceTable_->nbRequested();
  nbEventsPerSec_=nbEvents_-nbEventsLast_;
  nbEventsLast_  =nbEvents_;
  if (nbEventsPerSec_.value_>0) {
    if (nbEventsPerSec_<nbEventsPerSecMin_) nbEventsPerSecMin_=nbEventsPerSec_;
    if (nbEventsPerSec_>nbEventsPerSecMax_) nbEventsPerSecMax_=nbEventsPerSec_;
  }
  nbEventsPerSecAvg_=nbEvents_/nbMeasurements_;

  // number of MB per second measurement
  nbMBPerSec_=9.53674e-07*resourceTable_->nbBytes();
  nbMBTot_.value_+=nbMBPerSec_;
  if (nbMBPerSec_.value_>0) {
    if (nbMBPerSec_<nbMBPerSecMin_) nbMBPerSecMin_=nbMBPerSec_;
    if (nbMBPerSec_>nbMBPerSecMax_) nbMBPerSecMax_=nbMBPerSec_;
  }
  nbMBPerSecAvg_=nbMBTot_/nbMeasurements_;

  gui_->unlockInfoSpaces();

  // check if the event queue should be filled up again.
  if (itsTimeToAllocate()) {
    sendAllocate();
    nbTimeExpired_.value_++;
  }
  
  lock_.give();
}


//______________________________________________________________________________
void FUResourceBroker::initTimer()
{
  toolbox::task::getTimerFactory()->createTimer(sourceId_);
  toolbox::task::Timer *timer=toolbox::task::getTimerFactory()->getTimer(sourceId_);
  timer->stop();
}


//______________________________________________________________________________
void FUResourceBroker::startTimer()
{
  toolbox::task::Timer *timer =toolbox::task::getTimerFactory()->getTimer(sourceId_);
  if (0!=timer) {
    toolbox::TimeInterval oneSec(1.);
    toolbox::TimeVal      startTime=toolbox::TimeVal::gettimeofday();
    timer->start();
    timer->scheduleAtFixedRate(startTime,this,oneSec,gui_->monInfoSpace(),sourceId_);
  }
  else {
    LOG4CPLUS_ERROR(log_,"Could't start timer for performance measurements.");
  }
}


//______________________________________________________________________________
void FUResourceBroker::stopTimer()
{ 
  toolbox::task::Timer *timer =toolbox::task::getTimerFactory()->getTimer(sourceId_);
  if (0!=timer) timer->stop();
}


//______________________________________________________________________________
void FUResourceBroker::actionPerformed(xdata::Event& e)
{
  if (0==resourceTable_) return;
  
  gui_->lockInfoSpaces();
  
  if (e.type()=="ItemRetrieveEvent") {
    
    string item=dynamic_cast<xdata::ItemRetrieveEvent&>(e).itemName();
    
    if (item=="nbShmClients")      nbShmClients_     =resourceTable_->nbShmClients();
    if (item=="nbAllocatedEvents") nbAllocatedEvents_=resourceTable_->nbAllocated();
    if (item=="nbReceivedEvents")  nbReceivedEvents_ =resourceTable_->nbCompleted();
    if (item=="nbLostEvents")      nbLostEvents_     =resourceTable_->nbLost();
    if (item=="nbDataErrors")      nbDataErrors_     =resourceTable_->nbErrors();
    if (item=="nbCrcErrors")       nbCrcErrors_      =resourceTable_->nbCrcErrors();
  }
  
  if (e.type()=="ItemChangedEvent") {
    
    string item=dynamic_cast<xdata::ItemChangedEvent&>(e).itemName();
    
    if (item=="doCrcCheck") resourceTable_->setDoCrcCheck(doCrcCheck_);
  }
  
  gui_->unlockInfoSpaces();
}


//______________________________________________________________________________
void FUResourceBroker::configureAction(toolbox::Event::Reference e) 
  throw (toolbox::fsm::exception::Exception)
{
  // initialize resource table
  if (0==resourceTable_) {
    resourceTable_=new FUResourceTable(queueSize_,eventBufferSize_,shmMode_,log_);
  }
  else if (resourceTable_->nbResources()!=queueSize_) {
    resourceTable_->initialize(queueSize_,eventBufferSize_);
  }
  resourceTable_->resetCounters();
  
  // reset counters
  gui_->resetCounters();

  // establish connection to builder unit(s)
  connectToBUs();

  // initialze timer for nbEventsPerSec / nbMBPerSec measurements
  nbEventsPerSecMin_=10000;
  nbMBPerSecMin_    =1e06;
  nbMBPerSecMax_    =0.0;
  nbMeasurements_   =0;
  nbEventsLast_     =0;

  initTimer();
  
  LOG4CPLUS_INFO(log_,"FUResourceBroker -> CONFIGURED <-");
}


//______________________________________________________________________________
void FUResourceBroker::enableAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception)
{
  // request events from builder unit
  sendAllocate();
  
  LOG4CPLUS_INFO(log_,"FUResourceBroker -> ENABLED <-");
}


//______________________________________________________________________________
void FUResourceBroker::suspendAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception)
{
  lock_.take();
  LOG4CPLUS_INFO(log_,"FUResourceBroker -> SUSPENDED <-");
}


//______________________________________________________________________________
void FUResourceBroker::resumeAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception)
{
  lock_.give();
  LOG4CPLUS_INFO(log_,"FUResourceBroker -> RESUMED <-");
}


//______________________________________________________________________________
void FUResourceBroker::haltAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception)
{
  stopTimer();
  
  LOG4CPLUS_INFO(log_,"FUResourceBroker -> HALTED <-");
}


//______________________________________________________________________________
void FUResourceBroker::nullAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception)
{
  LOG4CPLUS_INFO(log_,"FUResourceBroker::nullAction() called.");
}


//______________________________________________________________________________
xoap::MessageReference FUResourceBroker::fireEvent(xoap::MessageReference msg)
  throw (xoap::exception::Exception)
{
  xoap::SOAPPart     part    =msg->getSOAPPart();
  xoap::SOAPEnvelope env     =part.getEnvelope();
  xoap::SOAPBody     body    =env.getBody();
  DOMNode           *node    =body.getDOMNode();
  DOMNodeList       *bodyList=node->getChildNodes();
  DOMNode           *command =0;
  string             commandName;
  
  for (UInt_t i=0;i<bodyList->getLength();i++) {
    command = bodyList->item(i);
    if(command->getNodeType() == DOMNode::ELEMENT_NODE) {
      commandName = xoap::XMLCh2String(command->getLocalName());
      return fsm_->processFSMCommand(commandName);
    }
  }
  XCEPT_RAISE(xoap::exception::Exception,"Command not found");
}


//______________________________________________________________________________
void FUResourceBroker::connectToBUs()
{
  if (0!=bu_.size()) return;
  
  typedef set<xdaq::ApplicationDescriptor*> AppDescSet_t;
  typedef AppDescSet_t::iterator            AppDescIter_t;
    
  AppDescSet_t buAppDescs=
    getApplicationContext()->getDefaultZone()->
    getApplicationDescriptors(buClassName_.toString());
  
  UInt_t maxBuInstance(0);
  for (AppDescIter_t it=buAppDescs.begin();it!=buAppDescs.end();++it)
    if ((*it)->getInstance()>maxBuInstance) maxBuInstance=(*it)->getInstance();
  
  bu_.resize(maxBuInstance+1);
  bu_.assign(bu_.size(),0);
  
  if (bu_.size()!=buAppDescs.size()) LOG4CPLUS_ERROR(log_,"maxBuInstance > #BUs!");
  
  bool buInstValid(false);
  
  for (UInt_t i=0;i<bu_.size();i++) {
    for (AppDescIter_t it=buAppDescs.begin();it!=buAppDescs.end();++it) {
      if (i==(*it)->getInstance()&&0==bu_[i]) {
	bu_[i]=new BUProxy(getApplicationDescriptor(),
			   *it, 
			   getApplicationContext(),
			   i2oPool_);
	if (i==buInstance_) buInstValid=true;
      }
    }
  }

  if (!buInstValid) LOG4CPLUS_ERROR(log_,"invalid buInstance! reset!!");
}


//______________________________________________________________________________
void FUResourceBroker::sendAllocate()
{
  if (bu_.size()<=buInstance_||bu_[buInstance_]==0) return;

  UInt_t    nbEvents=resourceTable_->nbFreeSlots();
  UIntVec_t fuResourceIds;
  for(UInt_t i=0;i<nbEvents;i++)
    fuResourceIds.push_back(resourceTable_->allocateResource());
  
  LOG4CPLUS_DEBUG(log_,"FUResourceBroker: sendAllocate(nbEvents="<<nbEvents<<")");
  
  bu_[buInstance_]->sendAllocate(fuResourceIds);

  nbPendingRequests_.value_+=nbEvents;
  nbAllocateSent_.value_++;
}


//______________________________________________________________________________
void FUResourceBroker::sendCollect(UInt_t fuResourceId)
{
  if (bu_.size()<=buInstance_||bu_[buInstance_]==0) return;
  
  bu_[buInstance_]->sendCollect(fuResourceId);
}


//______________________________________________________________________________
void FUResourceBroker::sendDiscard(UInt_t buResourceId)
{
  if (bu_.size()<=buInstance_||bu_[buInstance_]==0) return;

  lock_.take();
  bu_[buInstance_]->sendDiscard(buResourceId);
  nbProcessedEvents_.value_++;
  lock_.give();
}


//______________________________________________________________________________
void FUResourceBroker::I2O_FU_TAKE_Callback(toolbox::mem::Reference* bufRef)
{
  //if (fsm_->getCurrentState()!='E') {
  //bufRef->release();
  //return;
  //}

  // start the timer only upon receiving the first message
  if (nbTakeReceived_.value_==0) startTimer();
  
  nbTakeReceived_.value_++;

  bool eventComplete=resourceTable_->buildResource(bufRef);

  if (eventComplete&&doDropEvents_) {
    UInt_t evtNumber;
    UInt_t buResourceId;
    FEDRawDataCollection *fedColl=rqstEvent(evtNumber,buResourceId);
    if (!shmMode_) {
      sendDiscard(buResourceId);
      delete fedColl;
    }
  }
  
  if (resourceTable_->nbBuIdsToBeDiscarded()>0) {
    UInt_t buId;
    while (resourceTable_->popBuIdToBeDiscarded(buId)) {
      sendDiscard(buId);
      nbPendingRequests_.value_--;
    }
  }

  // check if it is time to ask the BU to fill up the event queue
  lock_.take();
  if (itsTimeToAllocate()) sendAllocate();
  lock_.give();
}


//______________________________________________________________________________
void FUResourceBroker::webPageRequest(xgi::Input *in,xgi::Output *out)
  throw (xgi::exception::Exception)
{
  string name=in->getenv("PATH_INFO");
  if (name.empty()) name="defaultWebPage";
  static_cast<xgi::MethodSignature*>(gui_->getMethod(name))->invoke(in,out);
}


//______________________________________________________________________________
bool FUResourceBroker::itsTimeToAllocate()
{
  UInt_t nbFreeSlots   =resourceTable_->nbFreeSlots();
  UInt_t nbFreeSlotsMax=queueSize_/2;
  if (nbFreeSlots>nbFreeSlotsMax) return true;
  return false;
}


//______________________________________________________________________________
void FUResourceBroker::exportParameters()
{
  assert(0!=gui_);
  
  gui_->addMonitorParam("url",                &url_);
  gui_->addMonitorParam("class",              &class_);
  gui_->addMonitorParam("instance",           &instance_);
  gui_->addMonitorParam("runNumber",          &runNumber_);
  gui_->addMonitorParam("stateName",          &fsm_->stateName_);
  gui_->addMonitorParam("nbShmClients",       &nbShmClients_);

  gui_->addMonitorParam("nbMBTot",            &nbMBTot_);
  gui_->addMonitorParam("nbMBPerSec",         &nbMBPerSec_);
  gui_->addMonitorParam("nbMBPerSecMin",      &nbMBPerSecMin_);
  gui_->addMonitorParam("nbMBPerSecMax",      &nbMBPerSecMax_);
  gui_->addMonitorParam("nbMBPerSecAvg",      &nbMBPerSecAvg_);

  gui_->addMonitorCounter("nbEvents",         &nbEvents_);
  gui_->addMonitorCounter("nbEventsPerSec",   &nbEventsPerSec_);
  gui_->addMonitorCounter("nbEventsPerSecMin",&nbEventsPerSecMin_);
  gui_->addMonitorCounter("nbEventsPerSecMax",&nbEventsPerSecMax_);
  gui_->addMonitorCounter("nbEventsPerSecAvg",&nbEventsPerSecAvg_);
  gui_->addMonitorCounter("nbAllocatedEvents",&nbAllocatedEvents_);
  gui_->addMonitorCounter("nbPendingRequests",&nbPendingRequests_);
  gui_->addMonitorCounter("nbReceivedEvents", &nbReceivedEvents_);
  gui_->addMonitorCounter("nbProcessedEvents",&nbProcessedEvents_);
  gui_->addMonitorCounter("nbLostEvents",     &nbLostEvents_);
  gui_->addMonitorCounter("nbDataErrors",     &nbDataErrors_);
  gui_->addMonitorCounter("nbCrcErrors",      &nbCrcErrors_);

  gui_->addStandardParam("shmMode",           &shmMode_);
  gui_->addStandardParam("eventBufferSize",   &eventBufferSize_);
  //gui_->addStandardParam("doDumpLostEvents",   &doDumpLostEvents_);
  gui_->addStandardParam("doDropEvents",      &doDropEvents_);
  gui_->addStandardParam("doCrcCheck",        &doCrcCheck_);
  gui_->addStandardParam("buClassName",       &buClassName_);
  gui_->addStandardParam("buInstance",        &buInstance_);
  gui_->addStandardParam("queueSize",         &queueSize_);
  
  gui_->addDebugCounter("nbAllocateSent",     &nbAllocateSent_);
  gui_->addDebugCounter("nbTakeReceived",     &nbTakeReceived_);
  gui_->addDebugCounter("nbTimeExpired",      &nbTimeExpired_);

  gui_->exportParameters();

  gui_->addItemRetrieveListener("nbShmClients",     this);
  gui_->addItemRetrieveListener("nbAllocatedEvents",this);
  gui_->addItemRetrieveListener("nbReceivedEvents", this);
  gui_->addItemRetrieveListener("nbLostEvents",     this);
  gui_->addItemRetrieveListener("nbDataErrors",     this);
  gui_->addItemRetrieveListener("nbCrcErrors",      this);


  gui_->addItemChangedListener("doCrcCheck",        this);
}


////////////////////////////////////////////////////////////////////////////////
// XDAQ instantiator implementation macro
////////////////////////////////////////////////////////////////////////////////

XDAQ_INSTANTIATOR_IMPL(FUResourceBroker)
