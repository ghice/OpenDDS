// -*- C++ -*-
//
// $Id$

#include "DCPS/DdsDcps_pch.h" //Only the _pch include should start with DCPS/
#include "DataReaderImpl.h"
#include "tao/ORB_Core.h"
#include "SubscriptionInstance.h"
#include "ReceivedDataElementList.h"
#include "DomainParticipantImpl.h"
#include "Service_Participant.h"
#include "Qos_Helper.h"
#include "TopicImpl.h"
#include "SubscriberImpl.h"
#include "Transient_Kludge.h"
#include "Util.h"
#include "RequestedDeadlineWatchdog.h"

#include "dds/DCPS/transport/framework/EntryExit.h"
#if !defined (DDS_HAS_MINIMUM_BIT)
#include "BuiltInTopicUtils.h"
#endif // !defined (DDS_HAS_MINIMUM_BIT)

#include "ace/Reactor.h"
#include "ace/Auto_Ptr.h"

#if !defined (__ACE_INLINE__)
# include "DataReaderImpl.inl"
#endif /* ! __ACE_INLINE__ */

namespace OpenDDS
{
  namespace DCPS
  {

#if 0
    // Emacs trick to align code with first column
    // This will cause emacs to emit bogus alignment message
    // For now just disregard them.
  }}
#endif


DataReaderImpl::DataReaderImpl (void) :
  rd_allocator_(0),
  qos_ (TheServiceParticipant->initial_DataReaderQos ()),
  reverse_sample_lock_(sample_lock_),
  next_handle_(0),
  topic_servant_ (0),
  topic_desc_(0),
  listener_mask_(DEFAULT_STATUS_KIND_MASK),
  fast_listener_ (0),
  participant_servant_ (0),
  domain_id_ (0),
  subscriber_servant_(0),
  subscription_id_ (0),
  n_chunks_ (TheServiceParticipant->n_chunks ()),
  reactor_(0),
  liveliness_lease_duration_ (ACE_Time_Value::zero),
  liveliness_timer_id_(-1),
  last_deadline_missed_total_count_ (0),
  watchdog_ (),
  is_bit_ (false),
  initialized_ (false)
{
  CORBA::ORB_var orb = TheServiceParticipant->get_ORB ();
  reactor_ = orb->orb_core()->reactor();

  liveliness_changed_status_.active_count = 0;
  liveliness_changed_status_.inactive_count = 0;
  liveliness_changed_status_.active_count_change = 0;
  liveliness_changed_status_.inactive_count_change = 0;

  requested_deadline_missed_status_.total_count = 0;
  requested_deadline_missed_status_.total_count_change = 0;
  requested_deadline_missed_status_.last_instance_handle =
    ::DDS::HANDLE_NIL;

  requested_incompatible_qos_status_.total_count = 0;
  requested_incompatible_qos_status_.total_count_change = 0;
  requested_incompatible_qos_status_.last_policy_id = 0;
  requested_incompatible_qos_status_.policies.length(0);

  subscription_match_status_.total_count = 0;
  subscription_match_status_.total_count_change = 0;
  subscription_match_status_.last_publication_handle =
    ::DDS::HANDLE_NIL;

  sample_lost_status_.total_count = 0;
  sample_lost_status_.total_count_change = 0;

  sample_rejected_status_.total_count = 0;
  sample_rejected_status_.total_count_change = 0;
  sample_rejected_status_.last_reason =
    ::DDS::REJECTED_BY_INSTANCE_LIMIT;
  sample_rejected_status_.last_instance_handle = ::DDS::HANDLE_NIL;
}

// This method is called when there are no longer any reference to the
// the servant.
DataReaderImpl::~DataReaderImpl (void)
{
  DBG_ENTRY_LVL ("DataReaderImpl","~DataReaderImpl",6);

  if (initialized_)
    {
      delete rd_allocator_;
    }
}

// this method is called when delete_datawriter is called.
void
DataReaderImpl::cleanup ()
{
  {
  ACE_GUARD (ACE_Recursive_Thread_Mutex,
    guard,
    this->sample_lock_);

  if (liveliness_timer_id_ != -1)
    {
      (void) reactor_->cancel_timer (this);
    }
  }

  topic_servant_->remove_entity_ref ();
  topic_servant_->_remove_ref ();
  dr_local_objref_ = ::DDS::DataReader::_nil();
  deactivate_remote_object(dr_remote_objref_.in());
  dr_remote_objref_ = DataReaderRemote::_nil();
}


void DataReaderImpl::init (
                           TopicImpl*         a_topic,
                           const ::DDS::DataReaderQos &  qos,
                           ::DDS::DataReaderListener_ptr a_listener,
                           DomainParticipantImpl*        participant,
                           SubscriberImpl*               subscriber,
                           ::DDS::DataReader_ptr         dr_objref,
               ::OpenDDS::DCPS::DataReaderRemote_ptr dr_remote_objref
                           )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  topic_servant_ = a_topic;
  topic_servant_->_add_ref ();

  topic_servant_->add_entity_ref ();

  CORBA::String_var topic_name = topic_servant_->get_name ();

#if !defined (DDS_HAS_MINIMUM_BIT)
  is_bit_ = ACE_OS::strcmp (topic_name.in (), BUILT_IN_PARTICIPANT_TOPIC) == 0
    || ACE_OS::strcmp (topic_name.in (), BUILT_IN_TOPIC_TOPIC) == 0
    || ACE_OS::strcmp (topic_name.in (), BUILT_IN_SUBSCRIPTION_TOPIC) == 0
    || ACE_OS::strcmp (topic_name.in (), BUILT_IN_PUBLICATION_TOPIC) == 0;
#endif // !defined (DDS_HAS_MINIMUM_BIT)

  qos_ = qos;
  listener_ = ::DDS::DataReaderListener::_duplicate (a_listener);

  if (! CORBA::is_nil (listener_.in()))
    {
      fast_listener_ = listener_.in ();
    }

  // Only store the participant pointer, since it is our "grand"
  // parent, we will exist as long as it does
  participant_servant_ = participant;
  domain_id_ = participant_servant_->get_domain_id ();
  topic_desc_ =
    participant_servant_->lookup_topicdescription(topic_name.in ());

  // Only store the subscriber pointer, since it is our parent, we
  // will exist as long as it does.
  subscriber_servant_ = subscriber;
  dr_local_objref_    = ::DDS::DataReader::_duplicate (dr_objref);
  dr_remote_objref_   =
    ::OpenDDS::DCPS::DataReaderRemote::_duplicate (dr_remote_objref );

  initialized_ = true;
}


void DataReaderImpl::add_associations (::OpenDDS::DCPS::RepoId yourId,
                                       const OpenDDS::DCPS::WriterAssociationSeq & writers)
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  DBG_ENTRY_LVL("DataReaderImpl","add_associations",6);

  if (DCPS_debug_level >= 1)
  {
    ACE_DEBUG ((LM_DEBUG, "(%P|%t)DataReaderImpl::add_associations "
      "bit %d local %d remote %d num remotes %d \n", is_bit_, yourId, writers[0].writerId, writers.length () ));
  }

  if (entity_deleted_ == true)
  {
    if (DCPS_debug_level >= 1)
      ACE_DEBUG ((LM_DEBUG,
      ACE_TEXT("(%P|%t) DataReaderImpl::add_associations")
      ACE_TEXT(" This is a deleted datareader, ignoring add.\n")));
    return;
  }

  ::DDS::InstanceHandleSeq handles;

  if (0 == subscription_id_)
  {
    // add_associations was invoked before DCSPInfoRepo::add_subscription() returned.
    subscription_id_ = yourId;
  }

  {
    ACE_GUARD (ACE_Recursive_Thread_Mutex, guard, this->publication_handle_lock_);

    // keep track of writers associate with this reader
    CORBA::ULong wr_len = writers.length ();
    for (CORBA::ULong i = 0; i < wr_len; i++)
    {
      PublicationId writer_id = writers[i].writerId;
      WriterInfo info(this, writer_id);
      if (bind(writers_, writer_id, info) != 0)
      {
        ACE_ERROR((LM_ERROR,
          "(%P|%t) DataReaderImpl::add_associations: "
          "ERROR: Unable to insert writer publisher_id %d.\n",
          writer_id));
      }
    }

    // add associations to the transport before using
    // Built-In Topic support and telling the listener.
    this->subscriber_servant_->add_associations(writers, this, qos_);

    if (liveliness_lease_duration_  != ACE_Time_Value::zero)
    {
      // this call will start the timer if it is not already set
      ACE_Time_Value now = ACE_OS::gettimeofday ();
      if (DCPS_debug_level >= 5)
        ACE_DEBUG((LM_DEBUG,
        "(%P|%t) DataReaderImpl::add_associations"
        " Starting/resetting liveliness timer for reader %d\n",
        subscription_id_ ));
      this->handle_timeout (now, this);
    }
    // else - no timer needed when LIVELINESS.lease_duration is INFINITE

    // Setup the requested deadline watchdog if the configured deadline
    // period is not the default (infinite).
    ::DDS::Duration_t const deadline_period = this->qos_.deadline.period;
    if (this->watchdog_.get () == 0
        && (deadline_period.sec != ::DDS::DURATION_INFINITY_SEC
            || deadline_period.nanosec != ::DDS::DURATION_INFINITY_NSEC))
    {
      ACE_auto_ptr_reset (this->watchdog_,
                          new RequestedDeadlineWatchdog (
                            this->reactor_,
                            this->sample_lock_,
                            this->qos_.deadline,
                            this,
                            this->dr_local_objref_.in (),
                            this->requested_deadline_missed_status_,
                            this->last_deadline_missed_total_count_));
    }
  }

  if (! is_bit_)
    {
      WriterIdSeq wr_ids;
      CORBA::ULong wr_len = writers.length ();
      wr_ids.length (wr_len);

      for (CORBA::ULong i = 0; i < wr_len; ++i)
      {
        wr_ids[i] = writers[i].writerId;
      }

      if (this->bit_lookup_instance_handles (wr_ids, handles) == false)
        return;

      ::DDS::DataReaderListener* listener
        = listener_for (::DDS::SUBSCRIPTION_MATCH_STATUS);

      ::DDS::SubscriptionMatchStatus subscription_match_status;

      ACE_GUARD (ACE_Recursive_Thread_Mutex, guard, this->publication_handle_lock_);
      wr_len = handles.length ();
      CORBA::ULong pub_len = publication_handles_.length();
      publication_handles_.length (pub_len + wr_len);

      for (CORBA::ULong i = 0; i < wr_len; i++)
      {
        subscription_match_status_.total_count ++;
        subscription_match_status_.total_count_change ++;
        publication_handles_[pub_len + i] = handles[i];

        if (bind(id_to_handle_map_, wr_ids[i], handles[i]) != 0)
        {
          ACE_DEBUG ((LM_DEBUG, "(%P|%t)ERROR: DataReaderImpl::add_associations "
            "insert %d - %X to id_to_handle_map_ failed \n", wr_ids[i], handles[i]));
          return;
        }
        subscription_match_status_.last_publication_handle = handles[i];
      }

      set_status_changed_flag (::DDS::SUBSCRIPTION_MATCH_STATUS, true);

      subscription_match_status = subscription_match_status_;

      if (listener != 0)
      {
        listener->on_subscription_match (dr_local_objref_.in (),
          subscription_match_status);

        // TBD - why does the spec say to change this but not
        // change the ChangeFlagStatus after a listener call?

        // Client will look at it so next time it looks the change should be 0
        subscription_match_status_.total_count_change = 0;

      }
    }
}


void DataReaderImpl::remove_associations (
                                          const OpenDDS::DCPS::WriterIdSeq & writers,
                                          ::CORBA::Boolean notify_lost
                                          )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  DBG_ENTRY_LVL("DataReaderImpl","remove_associations",6);

  if (DCPS_debug_level >= 1)
  {
    ACE_DEBUG ((LM_DEBUG, "(%P|%t)DataReaderImpl::remove_associations "
      "bit %d local %d remote %d num remotes %d \n", is_bit_, subscription_id_, writers[0], writers.length ()));
  }

  ::DDS::InstanceHandleSeq handles;

  ACE_GUARD (ACE_Recursive_Thread_Mutex, guard, this->publication_handle_lock_);

  WriterIdSeq updated_writers;

  //Remove the writers from writer list. If the supplied writer
  //is not in the cached writers list then it is already removed.
  //We just need remove the writers in the list that have not been
  //removed.
  CORBA::ULong wr_len = writers.length ();
  for (CORBA::ULong i = 0; i < wr_len; i++)
  {
    PublicationId writer_id = writers[i];

    WriterMapType::iterator it = this->writers_.find (writer_id);
    if (it != this->writers_.end())
    {
      it->second.removed ();
    }

    if (this->writers_.erase(writer_id) == 0)
    {
      if (DCPS_debug_level >= 1)
      {
        ACE_DEBUG((LM_DEBUG,
          "(%P|%t) DataReaderImpl::remove_associations: "
          "the writer %d was already removed.\n",
          writer_id));
      }
    }
    else
    {
      CORBA::Long len = updated_writers.length ();
      updated_writers.length (len + 1);
      updated_writers[len] = writer_id;
    }
  }

  wr_len = updated_writers.length ();

  // Return now if the supplied writers have been removed already.
  if (wr_len == 0)
  {
    return;
  }

  if (! is_bit_)
  {
    // The writer should be in the id_to_handle map at this time so
    // log with error.
    if (this->cache_lookup_instance_handles (updated_writers, handles) == false)
    {
      ACE_ERROR ((LM_ERROR, "(%P|%t)DataReaderImpl::remove_associations "
        "cache_lookup_instance_handles failed\n"));
      return;
    }

    CORBA::ULong pubed_len = publication_handles_.length();
    //CORBA::ULong wr_len = handles.length();
    for (CORBA::ULong wr_index = 0; wr_index < wr_len; wr_index++)
    {

      for (CORBA::ULong pubed_index = 0;
        pubed_index < pubed_len;
        pubed_index++)
      {
        if (publication_handles_[pubed_index] == handles[wr_index])
        {
          // move last element to this position.
          if (pubed_index < pubed_len - 1)
          {
            publication_handles_[pubed_index]
            = publication_handles_[pubed_len - 1];
          }
          pubed_len --;
          publication_handles_.length (pubed_len);
          break;
        }
      }
    }

    for (CORBA::ULong i = 0; i < wr_len; ++i)
    {
      id_to_handle_map_.erase(writers[i]);
    }
  }

  this->subscriber_servant_->remove_associations(updated_writers, this->subscription_id_);

  // If this remove_association is invoked when the InfoRepo
  // detects a lost writer then make a callback to notify
  // subscription lost.
  if (notify_lost)
  {
    this->notify_subscription_lost (updated_writers);
  }
}


void DataReaderImpl::remove_all_associations ()
{
  DBG_ENTRY_LVL("DataReaderImpl","remove_all_associations",6);

  OpenDDS::DCPS::WriterIdSeq writers;

  ACE_GUARD (ACE_Recursive_Thread_Mutex, guard, this->publication_handle_lock_);

  int size = writers_.size();
  writers.length(size);
  WriterMapType::iterator curr_writer = writers_.begin();
  WriterMapType::iterator end_writer = writers_.end();

  int i = 0;
  while (curr_writer != end_writer)
    {
      writers[i++] = curr_writer->first;
      ++curr_writer;
    }

  try
    {
      CORBA::Boolean dont_notify_lost = 0;
      if (0 < size)
        {
          remove_associations(writers, dont_notify_lost);
        }
    }
  catch (const CORBA::Exception&)
    {
    }
}


void DataReaderImpl::update_incompatible_qos (
    const OpenDDS::DCPS::IncompatibleQosStatus & status)
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  ::DDS::DataReaderListener* listener =
    listener_for (::DDS::REQUESTED_INCOMPATIBLE_QOS_STATUS);

  ACE_GUARD (ACE_Recursive_Thread_Mutex,
             guard,
             this->publication_handle_lock_);

  set_status_changed_flag(::DDS::REQUESTED_INCOMPATIBLE_QOS_STATUS,
                          true);

  // copy status and increment change
  requested_incompatible_qos_status_.total_count = status.total_count;
  requested_incompatible_qos_status_.total_count_change +=
    status.count_since_last_send;
  requested_incompatible_qos_status_.last_policy_id =
    status.last_policy_id;
  requested_incompatible_qos_status_.policies = status.policies;

  if (listener != 0)
    {
      listener->on_requested_incompatible_qos (dr_local_objref_.in (),
                                               requested_incompatible_qos_status_);


      // TBD - why does the spec say to change total_count_change but not
      // change the ChangeFlagStatus after a listener call?

      // client just looked at it so next time it looks the
      // change should be 0
      requested_incompatible_qos_status_.total_count_change = 0;
    }
}

::DDS::ReturnCode_t DataReaderImpl::delete_contained_entities ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  if (DCPS_debug_level >= 2)
    ACE_DEBUG ((LM_DEBUG,
                ACE_TEXT("(%P|%t) ")
                ACE_TEXT("DataReaderImpl::delete_contained_entities\n")));
  return ::DDS::RETCODE_OK;
}

::DDS::ReturnCode_t DataReaderImpl::set_qos (
                                             const ::DDS::DataReaderQos & qos
                                             )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  if (Qos_Helper::valid(qos) && Qos_Helper::consistent(qos))
  {
    if (qos_ == qos)
      return ::DDS::RETCODE_OK;

    if (enabled_.value())
    {
      if (! Qos_Helper::changeable (qos_, qos))
      {
        return ::DDS::RETCODE_IMMUTABLE_POLICY;
      }
      else
      {
        try
        {
          DCPSInfo_var repo = TheServiceParticipant->get_repository(domain_id_);
          ::DDS::SubscriberQos subscriberQos;
          this->subscriber_servant_->get_qos(subscriberQos);
          repo->update_subscription_qos(this->participant_servant_->get_domain_id(),
                                        this->participant_servant_->get_id(),
                                        this->subscription_id_,
                                        qos,
                                        subscriberQos);
        }
        catch (const CORBA::SystemException& sysex)
        {
          sysex._tao_print_exception (
            "ERROR: System Exception"
            " in DataReaderImpl::set_qos");
          return ::DDS::RETCODE_ERROR;
        }
        catch (const CORBA::UserException& userex)
        {
          userex._tao_print_exception (
            "ERROR:  Exception"
            " in DataReaderImpl::set_qos");
          return ::DDS::RETCODE_ERROR;
        }
      }
    }

    // Reset the deadline timer if the period has changed.
    if (qos_.deadline.period.sec != qos.deadline.period.sec
        || qos_.deadline.period.nanosec != qos.deadline.period.nanosec)
    {
      this->watchdog_->reset_interval (
        duration_to_time_value (qos.deadline.period));
    }

    qos_ = qos;

    return ::DDS::RETCODE_OK;
  }
  else
  {
    return ::DDS::RETCODE_INCONSISTENT_POLICY;
  }
}

void DataReaderImpl::get_qos (
                              ::DDS::DataReaderQos & qos
                              )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  qos = qos_;
}

::DDS::ReturnCode_t DataReaderImpl::set_listener (
                                                  ::DDS::DataReaderListener_ptr a_listener,
                                                  ::DDS::StatusKindMask mask
                                                  )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  listener_mask_ = mask;
  //note: OK to duplicate  and reference_to_servant a nil object ref
  listener_ = ::DDS::DataReaderListener::_duplicate(a_listener);
  fast_listener_ = listener_.in ();
  return ::DDS::RETCODE_OK;
}

::DDS::DataReaderListener_ptr DataReaderImpl::get_listener (
                                                            )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  return ::DDS::DataReaderListener::_duplicate (listener_.in ());
}


::DDS::TopicDescription_ptr DataReaderImpl::get_topicdescription (
                                                                  )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  return ::DDS::TopicDescription::_duplicate (topic_desc_.in ());
}

::DDS::Subscriber_ptr DataReaderImpl::get_subscriber (
                                                      )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  return ::DDS::Subscriber::_duplicate (subscriber_servant_);
}

::DDS::SampleRejectedStatus DataReaderImpl::get_sample_rejected_status (
                                                                        )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> justMe (this->sample_lock_);

  set_status_changed_flag (::DDS::SAMPLE_REJECTED_STATUS, false);
  ::DDS::SampleRejectedStatus status = sample_rejected_status_;
  sample_rejected_status_.total_count_change = 0;
  return status;
}

::DDS::LivelinessChangedStatus DataReaderImpl::get_liveliness_changed_status (
                                                                              )
  ACE_THROW_SPEC ((
                   CORBA::SystemException
                   ))
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> justMe (this->sample_lock_);

  set_status_changed_flag(::DDS::LIVELINESS_CHANGED_STATUS,
                          false);
  ::DDS::LivelinessChangedStatus status =
      liveliness_changed_status_;

  liveliness_changed_status_.active_count_change = 0;
  liveliness_changed_status_.inactive_count_change = 0;

  return status;
}

::DDS::RequestedDeadlineMissedStatus
DataReaderImpl::get_requested_deadline_missed_status ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> justMe (this->sample_lock_);

  set_status_changed_flag(::DDS::REQUESTED_DEADLINE_MISSED_STATUS,
                          false);

  this->requested_deadline_missed_status_.total_count_change =
    this->requested_deadline_missed_status_.total_count
    - this->last_deadline_missed_total_count_;

  // ::DDS::RequestedDeadlineMissedStatus::last_instance_handle field
  // is updated by the RequestedDeadlineWatchdog.

  // Update for next status check.
  this->last_deadline_missed_total_count_ =
    this->requested_deadline_missed_status_.total_count;

  ::DDS::RequestedDeadlineMissedStatus const status =
      requested_deadline_missed_status_;

  return status;
}

::DDS::RequestedIncompatibleQosStatus *
DataReaderImpl::get_requested_incompatible_qos_status ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> justMe (
    this->publication_handle_lock_);

  set_status_changed_flag(::DDS::REQUESTED_INCOMPATIBLE_QOS_STATUS,
                          false);
  ::DDS::RequestedIncompatibleQosStatus* status
      = new ::DDS::RequestedIncompatibleQosStatus;
  *status = requested_incompatible_qos_status_;
  requested_incompatible_qos_status_.total_count_change = 0;
  return status;
}

::DDS::SubscriptionMatchStatus
DataReaderImpl::get_subscription_match_status ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> justMe (
    this->publication_handle_lock_);

  set_status_changed_flag (::DDS::SUBSCRIPTION_MATCH_STATUS, false);
  ::DDS::SubscriptionMatchStatus status = subscription_match_status_;
  subscription_match_status_.total_count_change = 0;

  return status ;
}

::DDS::SampleLostStatus
DataReaderImpl::get_sample_lost_status ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  ACE_Guard<ACE_Recursive_Thread_Mutex> justMe (this->sample_lock_);

  set_status_changed_flag (::DDS::SAMPLE_LOST_STATUS, false);
  ::DDS::SampleLostStatus status = sample_lost_status_;
  sample_lost_status_.total_count_change = 0;
  return status;
}

::DDS::ReturnCode_t
DataReaderImpl::wait_for_historical_data (
    const ::DDS::Duration_t & /* max_wait */)
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  // Add your implementation here
  return 0;
}

::DDS::ReturnCode_t
DataReaderImpl::get_matched_publications (
    ::DDS::InstanceHandleSeq & publication_handles)
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  if (enabled_ == false)
    {
      ACE_ERROR_RETURN ((LM_ERROR,
                         ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::get_matched_publications, ")
                         ACE_TEXT(" Entity is not enabled. \n")),
                        ::DDS::RETCODE_NOT_ENABLED);
    }

  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex,
                    guard,
                    this->publication_handle_lock_,
                    ::DDS::RETCODE_ERROR);

  publication_handles = publication_handles_;

  return ::DDS::RETCODE_OK;
}

#if !defined (DDS_HAS_MINIMUM_BIT)
::DDS::ReturnCode_t
DataReaderImpl::get_matched_publication_data (
    ::DDS::PublicationBuiltinTopicData & publication_data,
    ::DDS::InstanceHandle_t publication_handle)
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  if (enabled_ == false)
    {
      ACE_ERROR_RETURN ((LM_ERROR,
                         ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::")
                         ACE_TEXT("get_matched_publication_data, ")
                         ACE_TEXT("Entity is not enabled. \n")),
                        ::DDS::RETCODE_NOT_ENABLED);
    }

  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex,
                    guard,
                    this->publication_handle_lock_,
                    ::DDS::RETCODE_ERROR);

  BIT_Helper_1 < ::DDS::PublicationBuiltinTopicDataDataReader,
    ::DDS::PublicationBuiltinTopicDataDataReader_var,
    ::DDS::PublicationBuiltinTopicDataSeq > hh;

  ::DDS::PublicationBuiltinTopicDataSeq data;

  ::DDS::ReturnCode_t ret
      = hh.instance_handle_to_bit_data(participant_servant_,
                                       BUILT_IN_PUBLICATION_TOPIC,
                                       publication_handle,
                                       data);
  if (ret == ::DDS::RETCODE_OK)
    {
      publication_data = data[0];
    }

  return ret;
}
#endif // !defined (DDS_HAS_MINIMUM_BIT)

::DDS::ReturnCode_t
DataReaderImpl::enable ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  if (qos_.history.kind == ::DDS::KEEP_ALL_HISTORY_QOS)
    {
      // The spec says qos_.history.depth is "has no effect"
      // when history.kind = KEEP_ALL so use max_samples_per_instance
      depth_ = qos_.resource_limits.max_samples_per_instance;
    }
  else // qos_.history.kind == ::DDS::KEEP_LAST_HISTORY_QOS
    {
      depth_ = qos_.history.depth;
    }

  if (depth_ == ::DDS::LENGTH_UNLIMITED)
    {
      // ::DDS::LENGTH_UNLIMITED is negative so make it a positive
      // value that is for all intents and purposes unlimited
      // and we can use it for comparisons.
      // use 2147483647L because that is the greatest value a signed
      // CORBA::Long can have.
      // WARNING: The client risks running out of memory in this case.
      depth_ = 2147483647L;
    }

  if (qos_.resource_limits.max_samples != ::DDS::LENGTH_UNLIMITED)
    {
      n_chunks_ = qos_.resource_limits.max_samples;
    }
  //else using value from Service_Participant

  // enable the type specific part of this DataReader
  this->enable_specific ();

  //Note: the QoS used to set n_chunks_ is Changable=No so
  // it is OK that we cannot change the size of our allocators.
  rd_allocator_ = new ReceivedDataAllocator(n_chunks_);
  if (DCPS_debug_level >= 2)
    ACE_DEBUG((LM_DEBUG,"(%P|%t) DataReaderImpl::enable"
               " Cached_Allocator_With_Overflow %x with %d chunks\n",
               rd_allocator_, n_chunks_));

  if ((qos_.liveliness.lease_duration.sec !=
       ::DDS::DURATION_INFINITY_SEC) ||
      (qos_.liveliness.lease_duration.nanosec !=
       ::DDS::DURATION_INFINITY_NSEC))
    {
      liveliness_lease_duration_ =
        duration_to_time_value (qos_.liveliness.lease_duration);
    }

  this->set_enabled ();

  CORBA::String_var name = topic_servant_->get_name ();

  subscriber_servant_->reader_enabled(
                      dr_remote_objref_.in (),
                      dr_local_objref_.in (),
                      this,
                                      name.in(),
                                      topic_servant_->get_id());

  return ::DDS::RETCODE_OK;
}

::DDS::StatusKindMask
DataReaderImpl::get_status_changes ()
  ACE_THROW_SPEC ((CORBA::SystemException))
{
  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex,
                    guard,
                    this->sample_lock_,
                    ::DDS::RETCODE_ERROR);

  return EntityImpl::get_status_changes ();
}


void
DataReaderImpl::writer_activity(PublicationId writer_id)
{
  // caller should have the sample_lock_ !!!

  WriterMapType::iterator iter = writers_.find(writer_id);
  if (iter != writers_.end())
    {
      ACE_Time_Value when = ACE_OS::gettimeofday ();
      iter->second.received_activity (when);
    }
  else
    // This may not be an error since it could happen that the sample
    // is delivered to the datareader after the write is dis-associated
    // with this datareader.
    ACE_DEBUG((LM_DEBUG,"(%P|%t) DataReaderImpl::writer_activity"
               " reader %d is not associated with writer %d\n",
               this->subscription_id_, writer_id));
}

void
DataReaderImpl::data_received(const ReceivedDataSample& sample)
{
  DBG_ENTRY_LVL("DataReaderImpl","data_received",6);

  // ensure some other thread is not changing the sample container
  // or statuses related to samples.
  ACE_GUARD (ACE_Recursive_Thread_Mutex, guard, this->sample_lock_);

  switch (sample.header_.message_id_)
    {
    case SAMPLE_DATA:
    case INSTANCE_REGISTRATION:
      {
        DataSampleHeader const & header = sample.header_;

        this->writer_activity(header.publication_id_);

        // "Pet the dog" so that it doesn't callback on the listener
        // the next time deadline timer expires.
        if (this->watchdog_.get ())
          this->watchdog_->signal ();

        // Verify data has not exceeded its lifespan.
        if (this->data_expired (header))
        {
          // Data expired.  Do not demarshal the data.  Simply allow
          // the caller to deallocate the data buffer.
          break;
        }

        // This also adds to the sample container
        this->dds_demarshal(sample);

        this->subscriber_servant_->data_received(this);
      }
      break;

    case DATAWRITER_LIVELINESS:
      {
        this->writer_activity(sample.header_.publication_id_);

        // tell all instances they got a liveliness message
        for (SubscriptionInstanceMapType::iterator iter = instances_.begin();
             iter != instances_.end();
             ++iter)
          {
            SubscriptionInstance *ptr = iter->second;

            ptr->instance_state_.lively (sample.header_.publication_id_);
          }

      }
      break;

    case DISPOSE_INSTANCE:
      this->writer_activity(sample.header_.publication_id_);
      this->dispose(sample);
      break;

    case UNREGISTER_INSTANCE:
      this->writer_activity(sample.header_.publication_id_);
      this->unregister(sample);
      break;


    default:
      ACE_ERROR((LM_ERROR,
                 "(%P|%t) %T ERRRO: DataReaderImpl::data_received"
                 "unexpected message_id = %d\n",
                 sample.header_.message_id_));
      break;
    }
}


SubscriberImpl* DataReaderImpl::get_subscriber_servant ()
{
  return subscriber_servant_;
}

RepoId DataReaderImpl::get_subscription_id() const
{
  return subscription_id_;
}

void DataReaderImpl::set_subscription_id(RepoId subscription_id)
{
  subscription_id_ = subscription_id;
}

char *
DataReaderImpl::get_topic_name() const
{
  return topic_servant_->get_name();
}

bool DataReaderImpl::have_sample_states(
                                        ::DDS::SampleStateMask sample_states) const
{
  //!!! caller should have acquired sample_lock_

  for (SubscriptionInstanceMapType::iterator iter = instances_.begin();
       iter != instances_.end();
       ++iter)
    {
      SubscriptionInstance *ptr = iter->second;

      for (ReceivedDataElement *item = ptr->rcvd_sample_.head_;
           item != 0; item = item->next_data_sample_)
        {
          if (item->sample_state_ & sample_states)
            {
              return true;
            }
        }
    }
  return false;
}

bool
DataReaderImpl::have_view_states(::DDS::ViewStateMask view_states) const
{
  //!!! caller should have acquired sample_lock_
  for (SubscriptionInstanceMapType::iterator iter = instances_.begin();
       iter != instances_.end();
       ++iter)
    {
      SubscriptionInstance *ptr = iter->second;

      if (ptr->instance_state_.view_state() & view_states)
        {
          return true;
        }
    }
  return false;
}

bool DataReaderImpl::have_instance_states(
                                          ::DDS::InstanceStateMask instance_states) const
{
  //!!! caller should have acquired sample_lock_
  for (SubscriptionInstanceMapType::iterator iter = instances_.begin();
       iter != instances_.end();
       ++iter)
    {
      SubscriptionInstance *ptr = iter->second;

      if (ptr->instance_state_.instance_state() & instance_states)
        {
          return true;
        }
    }
  return false;
}

::DDS::DataReaderListener*
DataReaderImpl::listener_for (::DDS::StatusKind kind)
{
  // per 2.1.4.3.1 Listener Access to Plain Communication Status
  // use this entities factory if listener is mask not enabled
  // for this kind.
  if ((listener_mask_ & kind) == 0)
    {
      return subscriber_servant_->listener_for (kind);
    }
  else
    {
      return fast_listener_;
    }
}

// zero-copy version of this metod
void
DataReaderImpl::sample_info(::DDS::SampleInfoSeq & info_seq,
                            //x ::OpenDDS::DCPS::SampleInfoZCSeq & info_seq,
                            size_t start_idx,
                            size_t count,
                            ReceivedDataElement *ptr)
{
  size_t end_idx = start_idx + count - 1;
  for (size_t i = start_idx; i <= end_idx; i++)
    {
      info_seq[i].sample_rank = count - (i - start_idx + 1);

      // generation_rank =
      //    (MRSIC.disposed_generation_count +
      //     MRSIC.no_writers_generation_count)
      //  - (S.disposed_generation_count +
      //     S.no_writers_generation_count)
      //
      //  info_seq[end_idx] == MRSIC
      //  info_seq[i].generation_rank ==
      //      (S.disposed_generation_count +
      //      (S.no_writers_generation_count) -- calculated in
      //            InstanceState::sample_info

      info_seq[i].generation_rank =
        (info_seq[end_idx].disposed_generation_count +
         info_seq[end_idx].no_writers_generation_count) -
        info_seq[i].generation_rank;

      // absolute_generation_rank =
      //     (MRS.disposed_generation_count +
      //      MRS.no_writers_generation_count)
      //   - (S.disposed_generation_count +
      //      S.no_writers_generation_count)
      //
      // ptr == MRS
      // info_seq[i].absolute_generation_rank ==
      //    (S.disposed_generation_count +
      //     S.no_writers_generation_count)-- calculated in
      //            InstanceState::sample_info
      //
      info_seq[i].absolute_generation_rank =
        (ptr->disposed_generation_count_ +
         ptr->no_writers_generation_count_) -
        info_seq[i].absolute_generation_rank;
    }
}

//void DataReaderImpl::sample_info(::DDS::SampleInfoSeq & info_seq,
//                               size_t start_idx, size_t count,
//                               ReceivedDataElement *ptr)
//{
//  size_t end_idx = start_idx + count - 1;
//  for (size_t i = start_idx; i <= end_idx; i++)
//    {
//      info_seq[i].sample_rank = count - (i - start_idx + 1);
//
//      // generation_rank =
//      //    (MRSIC.disposed_generation_count +
//      //     MRSIC.no_writers_generation_count)
//      //  - (S.disposed_generation_count +
//      //     S.no_writers_generation_count)
//      //
//      //  info_seq[end_idx] == MRSIC
//      //  info_seq[i].generation_rank ==
//      //      (S.disposed_generation_count +
//      //      (S.no_writers_generation_count) -- calculated in
//      //            InstanceState::sample_info
//
//      info_seq[i].generation_rank =
//      (info_seq[end_idx].disposed_generation_count +
//       info_seq[end_idx].no_writers_generation_count) -
//      info_seq[i].generation_rank;
//
//      // absolute_generation_rank =
//      //     (MRS.disposed_generation_count +
//      //      MRS.no_writers_generation_count)
//      //   - (S.disposed_generation_count +
//      //      S.no_writers_generation_count)
//      //
//      // ptr == MRS
//      // info_seq[i].absolute_generation_rank ==
//      //    (S.disposed_generation_count +
//      //     S.no_writers_generation_count)-- calculated in
//      //            InstanceState::sample_info
//      //
//      info_seq[i].absolute_generation_rank =
//      (ptr->disposed_generation_count_ +
//       ptr->no_writers_generation_count_) -
//      info_seq[i].absolute_generation_rank;
//    }
//}

void DataReaderImpl::sample_info(::DDS::SampleInfo & sample_info,
                                 ReceivedDataElement *ptr)
{

  sample_info.sample_rank = 0;

  // generation_rank =
  //    (MRSIC.disposed_generation_count +
  //     MRSIC.no_writers_generation_count)
  //  - (S.disposed_generation_count +
  //     S.no_writers_generation_count)
  //
  sample_info.generation_rank =
    (sample_info.disposed_generation_count +
     sample_info.no_writers_generation_count) -
    sample_info.generation_rank;

  // absolute_generation_rank =
  //     (MRS.disposed_generation_count +
  //      MRS.no_writers_generation_count)
  //   - (S.disposed_generation_count +
  //      S.no_writers_generation_count)
  //
  sample_info.absolute_generation_rank =
    (ptr->disposed_generation_count_ +
     ptr->no_writers_generation_count_) -
    sample_info.absolute_generation_rank;
}

CORBA::Long DataReaderImpl::total_samples() const
{
  //!!! caller should have acquired sample_lock_
  CORBA::Long count(0);
  for (SubscriptionInstanceMapType::iterator iter = instances_.begin();
       iter != instances_.end();
       ++iter)
    {
      SubscriptionInstance *ptr = iter->second;

      count += ptr->rcvd_sample_.size_;
    }

  return count;
}

int
DataReaderImpl::handle_timeout (const ACE_Time_Value &tv,
                                const void * arg)
{
  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex,
    guard,
    this->sample_lock_,
    ::DDS::RETCODE_ERROR);

  if (liveliness_timer_id_ != -1)
  {
    if (arg == this)
    {

      if (DCPS_debug_level >= 5)
        ACE_DEBUG((LM_DEBUG,
        "(%P|%t) DataReaderImpl::handle_timeout"
        " canceling timer for reader %d\n",
        subscription_id_ ));

      // called from add_associations and there is already a timer
      // so cancel the existing timer.
      if (reactor_->cancel_timer (this->liveliness_timer_id_, &arg) == -1)
      {
        // this could fail because the reactor's call and
        // the add_associations' call to this could overlap
        // so it is not a failure.
        ACE_DEBUG((LM_DEBUG,
          ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::handle_timeout, ")
          ACE_TEXT(" %p. \n"), "cancel_timer" ));
      }
      liveliness_timer_id_ = -1;
    }
  }

  ACE_Time_Value smallest(ACE_Time_Value::max_time);
  ACE_Time_Value next_absolute;
  int alive_writers = 0;

  // Iterate over each writer to this reader
  for (WriterMapType::iterator iter = writers_.begin();
    iter != writers_.end();
    ++iter)
  {
    // deal with possibly not being alive or
    // tell when it will not be alive next (if no activity)
    next_absolute = iter->second.check_activity(tv);

    if (next_absolute != ACE_Time_Value::max_time)
    {
      alive_writers++;
      if (next_absolute < smallest)
      {
        smallest = next_absolute;
      }
    }
  }

  if (DCPS_debug_level >= 5)
    ACE_DEBUG((LM_DEBUG,
    "(%P|%t) %T DataReaderImpl::handle_timeout"
    " reader %d has %d live writers; from_reator=%d\n",
    subscription_id_, alive_writers, arg == this ? 0 : 1));


  if (alive_writers)
  {
    ACE_Time_Value relative;
    ACE_Time_Value now = ACE_OS::gettimeofday ();
    // compare the time now with the earliest(smallest) deadline we found
    if (now < smallest)
      relative = smallest - now;
    else
      relative = ACE_Time_Value(0,1); // ASAP

   liveliness_timer_id_ = reactor_->schedule_timer(this, 0, relative);
    if (liveliness_timer_id_ == -1)
    {
      ACE_ERROR((LM_ERROR,
        ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::handle_timeout, ")
        ACE_TEXT(" %p. \n"), "schedule_timer" ));
    }
  }
  else {
    // no live writers so no need to schedule a timer
    // but be sure we don't try to cancel the timer later.
    liveliness_timer_id_ = -1;
  }

  return 0;
}


void
DataReaderImpl::release_instance (::DDS::InstanceHandle_t handle)
{
  ACE_GUARD (ACE_Recursive_Thread_Mutex, guard, this->sample_lock_);
  SubscriptionInstance* instance = this->get_handle_instance (handle);

  if (instance == 0)
  {
    ACE_ERROR ((LM_ERROR, "(%P|%t)DataReaderImpl::release_instance "
      "could not find the instance by handle %d\n", handle));
    return;
  }

  if (instance->rcvd_sample_.size_ == 0)
  {
    this->instances_.erase (handle);
    this->release_instance_i (handle);
  }
  else
  {
    ACE_ERROR ((LM_ERROR, "(%P|%t)DataReaderImpl::release_instance "
      "Can not release the instance (handle=%d) since it still has samples.\n",
      handle));
  }
}


OpenDDS::DCPS::WriterInfo::WriterInfo ()
  : last_liveliness_activity_time_(ACE_OS::gettimeofday()),
    state_(NOT_SET),
    reader_(0),
    writer_id_(0)
{
}

OpenDDS::DCPS::WriterInfo::WriterInfo (DataReaderImpl* reader,
                                   PublicationId   writer_id)
  : last_liveliness_activity_time_(ACE_OS::gettimeofday()),
    state_(NOT_SET),
    reader_(reader),
    writer_id_(writer_id)
{
  if (DCPS_debug_level >= 5)
    ACE_DEBUG((LM_DEBUG,"(%P|%t) WriterInfo::WriterInfo"
               " added writer %d to reader %d",
               writer_id_, reader_->subscription_id_));
}


ACE_Time_Value
OpenDDS::DCPS::WriterInfo::check_activity (const ACE_Time_Value& now)
{
  ACE_Time_Value expires_at = ACE_Time_Value::max_time;

  // We only need check the liveliness with the non-zero liveliness_lease_duration_.
  if (state_ != DEAD && reader_->liveliness_lease_duration_ != ACE_Time_Value::zero)
  {
    expires_at = this->last_liveliness_activity_time_ +
      reader_->liveliness_lease_duration_;
    if (expires_at <= now)
    {
      // let all instances know this write is not alive.
      reader_->writer_became_dead(writer_id_, now, state_);
      expires_at = ACE_Time_Value::max_time;
    }
  }
  return expires_at;
}


void OpenDDS::DCPS::WriterInfo::removed ()
{
  reader_->writer_removed (writer_id_, this->state_);
}

void
DataReaderImpl::writer_removed (PublicationId   writer_id,
             WriterInfo::WriterState& state)
{
  if (DCPS_debug_level >= 5)
    ACE_DEBUG((LM_DEBUG,
               "(%P|%t)%T  DataReaderImpl::writer_removed reader %d to writer %d\n",
               this->subscription_id_, writer_id));

  bool liveliness_changed = false;
  if (state == WriterInfo::ALIVE)
  {
    -- liveliness_changed_status_.active_count;
    -- liveliness_changed_status_.active_count_change;
    liveliness_changed = true;
  }

  if (state == WriterInfo::DEAD)
  {
    -- liveliness_changed_status_.inactive_count;
    -- liveliness_changed_status_.inactive_count_change;
    liveliness_changed = true;
  }

  if (liveliness_changed)
    this->notify_liveliness_change ();
}

void
DataReaderImpl::writer_became_alive (PublicationId writer_id,
                                     const ACE_Time_Value& /* when */,
                                     WriterInfo::WriterState& state)
{
  if (DCPS_debug_level >= 5)
    ACE_DEBUG((LM_DEBUG,
               "(%P|%t)%T  DataReaderImpl::writer_became_alive reader %d to writer %d\n",
               this->subscription_id_, writer_id));

  // caller should already have the samples_lock_ !!!

  // NOTE: each instance will change to ALIVE_STATE when they receive a sample

  bool liveliness_changed = false;
  if (state != WriterInfo::ALIVE)
  {
    liveliness_changed_status_.active_count++;
    liveliness_changed_status_.active_count_change++;
    liveliness_changed = true;
  }

  if (state == WriterInfo::DEAD)
  {
    liveliness_changed_status_.inactive_count--;
    liveliness_changed_status_.inactive_count_change--;
    liveliness_changed = true;
  }

  set_status_changed_flag(::DDS::LIVELINESS_CHANGED_STATUS, true);

  if (liveliness_changed_status_.active_count < 0)
    {
      ACE_ERROR ((LM_ERROR,
                  ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::writer_became_alive, ")
                  ACE_TEXT(" invalid liveliness_changed_status active count - %d.\n"),
      liveliness_changed_status_.active_count));
      return;
    }
  if (liveliness_changed_status_.inactive_count < 0)
    {
      ACE_ERROR ((LM_ERROR,
                  ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::writer_became_alive, ")
                  ACE_TEXT(" invalid liveliness_changed_status inactive count - %d .\n"),
      liveliness_changed_status_.inactive_count));
      return;
    }

  // Change the state to ALIVE since handle_timeout may call writer_became_dead
  // which need the current state info.
  state = WriterInfo::ALIVE;

  // Call listener only when there are liveliness status changes.
  if (liveliness_changed)
  {
    this->notify_liveliness_change ();
  }

  // this call will start the livilness timer if it is not already set
  ACE_Time_Value now = ACE_OS::gettimeofday ();
  this->handle_timeout (now, this);
}

void
DataReaderImpl::writer_became_dead (PublicationId   writer_id,
                                    const ACE_Time_Value& when,
            WriterInfo::WriterState& state)
{
  if (DCPS_debug_level >= 5)
    ACE_DEBUG((LM_DEBUG,
               "(%P|%t) DataReaderImpl::writer_became_dead reader %d to writer %d state %d\n",
               this->subscription_id_, writer_id, state));

  // caller should already have the samples_lock_ !!!
  bool liveliness_changed = false;

  if (state == OpenDDS::DCPS::WriterInfo::NOT_SET)
  {
    liveliness_changed_status_.inactive_count++;
    liveliness_changed_status_.inactive_count_change++;
    liveliness_changed = true;
  }

  if (state == WriterInfo::ALIVE)
  {
    liveliness_changed_status_.active_count--;
    liveliness_changed_status_.active_count_change--;
    liveliness_changed_status_.inactive_count++;
    liveliness_changed_status_.inactive_count_change++;
    liveliness_changed = true;
  }

  //update the state to DEAD.
  state = WriterInfo::DEAD;

  set_status_changed_flag(::DDS::LIVELINESS_LOST_STATUS,
    true);

  if (liveliness_changed_status_.active_count < 0)
    {
      ACE_ERROR ((LM_ERROR,
                  ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::writer_became_dead, ")
                  ACE_TEXT(" invalid liveliness_changed_status active count - %d.\n"),
      liveliness_changed_status_.active_count));
      return;
    }
  if (liveliness_changed_status_.inactive_count < 0)
    {
      ACE_ERROR ((LM_ERROR,
                  ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::writer_became_dead, ")
                  ACE_TEXT(" invalid liveliness_changed_status inactive count - %d.\n"),
      liveliness_changed_status_.inactive_count));
      return;
    }

    SubscriptionInstanceMapType::iterator iter = instances_.begin();
    SubscriptionInstanceMapType::iterator next = iter;

    while (iter != instances_.end())
    {
      ++next;
      SubscriptionInstance *ptr = iter->second;

      ptr->instance_state_.writer_became_dead (
        writer_id, liveliness_changed_status_.active_count, when);

      iter = next;
    }


  // Call listener only when there are liveliness status changes.
  if (liveliness_changed)
  {
    this->notify_liveliness_change ();
  }
}


int
DataReaderImpl::handle_close (ACE_HANDLE,
                              ACE_Reactor_Mask)
{
  //this->_remove_ref ();
  return 0;
}


void
DataReaderImpl::set_sample_lost_status(
                                       const ::DDS::SampleLostStatus& status)
{
  //!!! caller should have acquired sample_lock_
  sample_lost_status_ = status;
}


void
DataReaderImpl::set_sample_rejected_status(
                                           const ::DDS::SampleRejectedStatus& status)
{
  //!!! caller should have acquired sample_lock_
  sample_rejected_status_ = status;
}


void DataReaderImpl::dispose(const ReceivedDataSample& /* sample */)
{
  ACE_DEBUG ((LM_DEBUG, "(%P|%T)BIT dispose\n"));
}


void DataReaderImpl::unregister(const ReceivedDataSample& /* sample */)
{
  ACE_DEBUG ((LM_DEBUG, "(%P|%T)BIT unregister\n"));
}


SubscriptionInstance*
DataReaderImpl::get_handle_instance (::DDS::InstanceHandle_t handle)
{
  SubscriptionInstanceMapType::iterator iter = instances_.find(handle);
  if (iter == instances_.end())
    {
      ACE_ERROR ((LM_ERROR,
                  ACE_TEXT("(%P|%t) ERROR: ")
                  ACE_TEXT("DataReaderImpl::get_handle_instance, ")
                  ACE_TEXT("lookup for %d failed\n"),
                  handle));
      return 0;
    } // if (0 != instances_.find(handle, instance))
  return iter->second;
}


::DDS::InstanceHandle_t
DataReaderImpl::get_next_handle ()
{
  return next_handle_++;
}


void
DataReaderImpl::notify_subscription_disconnected (const WriterIdSeq& pubids)
{
  DBG_ENTRY_LVL("DataReaderImpl","notify_subscription_disconnected",6);
  if (! this->is_bit_)
    {
      // Narrow to DDS::DCPS::DataReaderListener. If a DDS::DataReaderListener
      // is given to this DataReader then narrow() fails.
      DataReaderListener_var the_listener
        = DataReaderListener::_narrow (this->listener_.in ());
      if (! CORBA::is_nil (the_listener.in ()))
      {
        SubscriptionLostStatus status;

        // Since this callback may come after remove_association which removes
        // the writer from id_to_handle map, we can ignore this error.
        this->cache_lookup_instance_handles (pubids, status.publication_handles);
        the_listener->on_subscription_disconnected (this->dr_local_objref_.in (),
        status);
      }
    }
}


void
DataReaderImpl::notify_subscription_reconnected (const WriterIdSeq& pubids)
{
  DBG_ENTRY_LVL("DataReaderImpl","notify_subscription_reconnected",6);

  if (! this->is_bit_)
  {
    // Narrow to DDS::DCPS::DataReaderListener. If a DDS::DataReaderListener
    // is given to this DataReader then narrow() fails.
    DataReaderListener_var the_listener
      = DataReaderListener::_narrow (this->listener_.in ());
    if (! CORBA::is_nil (the_listener.in ()))
    {
      SubscriptionLostStatus status;

      // If it's reconnected then the reader should be in id_to_handle map otherwise
      // log with an error.
      if (this->cache_lookup_instance_handles (pubids, status.publication_handles) == false)
      {
        ACE_ERROR ((LM_ERROR, "(%P|%t)DataReaderImpl::notify_subscription_reconnected "
          "cache_lookup_instance_handles failed\n"));
      }
      the_listener->on_subscription_reconnected (this->dr_local_objref_.in (),
        status);
    }
  }
}


void
DataReaderImpl::notify_subscription_lost (const WriterIdSeq& pubids)
{
  DBG_ENTRY_LVL("DataReaderImpl","notify_subscription_lost",6);

  if (! this->is_bit_)
  {
    // Narrow to DDS::DCPS::DataReaderListener. If a DDS::DataReaderListener
    // is given to this DataReader then narrow() fails.
    DataReaderListener_var the_listener
      = DataReaderListener::_narrow (this->listener_.in ());
    if (! CORBA::is_nil (the_listener.in ()))
    {
      SubscriptionLostStatus status;

      // Since this callback may come after remove_association which removes
      // the writer from id_to_handle map, we can ignore this error.
      this->cache_lookup_instance_handles (pubids, status.publication_handles);
      the_listener->on_subscription_lost (this->dr_local_objref_.in (),
        status);
    }
  }
}


void
DataReaderImpl::notify_connection_deleted ()
{
  DBG_ENTRY_LVL("DataReaderImpl","notify_connection_deleted",6);

  // Narrow to DDS::DCPS::DataWriterListener. If a DDS::DataWriterListener
  // is given to this DataWriter then narrow() fails.
  DataReaderListener_var the_listener = DataReaderListener::_narrow (this->listener_.in ());

  if (! CORBA::is_nil (the_listener.in ()))
    the_listener->on_connection_deleted (this->dr_local_objref_.in ());
}

bool
DataReaderImpl::bit_lookup_instance_handles (const WriterIdSeq& ids,
                                              ::DDS::InstanceHandleSeq & hdls)
{
  if (TheServiceParticipant->get_BIT () == true && ! TheTransientKludge->is_enabled ())
  {
#if !defined (DDS_HAS_MINIMUM_BIT)
    BIT_Helper_2 < ::DDS::PublicationBuiltinTopicDataDataReader,
      ::DDS::PublicationBuiltinTopicDataDataReader_var,
      ::DDS::PublicationBuiltinTopicDataSeq,
      WriterIdSeq > hh;

    ::DDS::ReturnCode_t ret
      = hh.repo_ids_to_instance_handles(participant_servant_,
      BUILT_IN_PUBLICATION_TOPIC,
      ids,
      hdls);

    if (ret != ::DDS::RETCODE_OK)
    {
      ACE_ERROR ((LM_ERROR,
        ACE_TEXT("(%P|%t) ERROR: DataReaderImpl::bit_lookup_instance_handles failed \n")));
      return false;
    }
#endif // !defined (DDS_HAS_MINIMUM_BIT)
  }
  else
  {
    CORBA::ULong num_wrts = ids.length ();
    hdls.length (num_wrts);
    for (CORBA::ULong i = 0; i < num_wrts; i++)
    {
      hdls[i] = ids[i];
    }
  }

  return true;
}

bool
DataReaderImpl::cache_lookup_instance_handles (const WriterIdSeq& ids,
                                              ::DDS::InstanceHandleSeq & hdls)
{
  bool ret = true;
  CORBA::ULong num_ids = ids.length ();
  for (CORBA::ULong i = 0; i < num_ids; ++i)
  {
    RepoIdToHandleMap::iterator iter = id_to_handle_map_.find(ids[i]);
    hdls.length (i + 1);
    if (iter == id_to_handle_map_.end())
    {
      ACE_DEBUG ((LM_WARNING, "(%P|%t)DataReaderImpl::cache_lookup_instance_handles "
        "could not find instance handle for writer %d\n", ids[i]));
      hdls[i] = -1;
      ret = false;
    }
    else
    {
      hdls[i] = iter->second;
    }
  }

  return ret;
}

bool
DataReaderImpl::data_expired (DataSampleHeader const & header) const
{
  // @@ Is getting the LIFESPAN value from the Topic sufficient?
  ::DDS::TopicQos topic_qos;
  this->topic_servant_->get_qos (topic_qos);

  ::DDS::LifespanQosPolicy const & lifespan = topic_qos.lifespan;

  if (lifespan.duration.sec != ::DDS::DURATION_INFINITY_SEC
      || lifespan.duration.nanosec != ::DDS::DURATION_INFINITY_NSEC)
  {
    // Finite lifespan.  Check if data has expired.

    ::DDS::Time_t const tmp =
      {
        header.source_timestamp_sec_ + lifespan.duration.sec,
        header.source_timestamp_nanosec_ + lifespan.duration.nanosec
      };

    // We assume that the publisher host's clock and subcriber host's
    // clock are synchronized (allowed by the spec).
    ACE_Time_Value const now (ACE_OS::gettimeofday ());
    ACE_Time_Value const expiration_time (
      OpenDDS::DCPS::time_to_time_value (tmp));
    if (now >= expiration_time)
    {
      if (DCPS_debug_level >= 8)
      {
        ACE_Time_Value const diff (now - expiration_time);
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("OpenDDS (%P|%t) Received data ")
                   ACE_TEXT("expired by %d seconds, %d microseconds.\n"),
                   diff.sec (),
                   diff.usec ()));
      }

      return true;  // Data expired.
    }
  }

  return false;
}

bool DataReaderImpl::is_bit () const
{
  return this->is_bit_;
}

int
DataReaderImpl::num_zero_copies()
{
  int loans = 0;
  ACE_GUARD_RETURN (ACE_Recursive_Thread_Mutex,
                    guard,
                    this->sample_lock_,
                    1 /* assume we have loans */);

  for (SubscriptionInstanceMapType::iterator iter = instances_.begin();
       iter != instances_.end();
       ++iter)
    {
      SubscriptionInstance *ptr = iter->second;

      for (OpenDDS::DCPS::ReceivedDataElement *item = ptr->rcvd_sample_.head_;
            item != 0; item = item->next_data_sample_)
        {
            loans += item->zero_copy_cnt_;
        }
    }

    return loans;
}


void DataReaderImpl::notify_liveliness_change()
{
  ::DDS::DataReaderListener* listener
    = listener_for (::DDS::LIVELINESS_CHANGED_STATUS);
  if (listener != 0)
  {
    listener->on_liveliness_changed (dr_local_objref_.in (),
      liveliness_changed_status_);

    liveliness_changed_status_.active_count_change = 0;
    liveliness_changed_status_.inactive_count_change = 0;
  }
}

} // namespace DCPS
} // namespace OpenDDS