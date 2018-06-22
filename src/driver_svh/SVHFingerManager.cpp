// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// This file is part of the SCHUNK SVH Driver suite.
//
// This program is free software licensed under the LGPL
// (GNU LESSER GENERAL PUBLIC LICENSE Version 3).
// You can find a copy of this license in LICENSE folder in the top
// directory of the source code.
//
// © Copyright 2014 SCHUNK Mobile Greifsysteme GmbH, Lauffen/Neckar Germany
// © Copyright 2014 FZI Forschungszentrum Informatik, Karlsruhe, Germany
//
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Lars Pfotzer
 * \author  Georg Heppner
 * \date    2014-01-30
 * \date    2014-08-20
 *
 * This file contains the Finger Manager
 * that is managing the Schunk five finger hand on a high level.
 * The fingermanager is responsible to filter all calls and only make
 * allowed calls to the controller. The fingermanager is responsible
 * for storing any kind of configuration (like current controller settings).
 * The fingermanager is also responsible to poll the controller for continious data (if wanted)
 *
 */
//----------------------------------------------------------------------
#include <driver_svh/Logging.h>
#include <driver_svh/SVHFingerManager.h>

#include <icl_core/TimeStamp.h>

#include <boost/bind/bind.hpp>

namespace driver_svh {

SVHFingerManager::SVHFingerManager(const std::vector<bool> &disable_mask, const uint32_t &reset_timeout) :
  m_controller(new SVHController()),
  m_feedback_thread(),
  m_connected(false),
  m_connection_feedback_given(false),
  m_homing_timeout(10),
  m_ticks2rad(0),
  m_position_min(eSVH_DIMENSION, 0),
  m_position_max(eSVH_DIMENSION, 0),
  m_position_home(eSVH_DIMENSION, 0),
  m_is_homed(eSVH_DIMENSION, false),
  m_is_switched_off(eSVH_DIMENSION,false),
  m_diagnostic_encoder_state(eSVH_DIMENSION,false),
  m_diagnostic_current_state(eSVH_DIMENSION,false),
  m_diagnostic_current_maximum(eSVH_DIMENSION, 0),
  m_diagnostic_current_minimum(eSVH_DIMENSION, 0),
  m_diagnostic_position_maximum(eSVH_DIMENSION, 0),
  m_diagnostic_position_minimum(eSVH_DIMENSION, 0),
  m_diagnostic_deadlock(eSVH_DIMENSION, 0),
  m_movement_state(eST_DEACTIVATED),
  m_reset_speed_factor(0.2),
  m_reset_timeout(reset_timeout),
  m_current_settings(eSVH_DIMENSION),
  m_current_settings_given(eSVH_DIMENSION,false),
  m_position_settings(eSVH_DIMENSION),
  m_position_settings_given(eSVH_DIMENSION,false),
  m_home_settings(eSVH_DIMENSION),
  m_serial_device("/dev/ttyUSB0")
{
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
  m_ws_broadcaster = boost::shared_ptr<icl_comm::websocket::WsBroadcaster>(new icl_comm::websocket::WsBroadcaster(icl_comm::websocket::WsBroadcaster::eRT_SVH));
  if (m_ws_broadcaster)
  {
    // Register a custom handler for received JSON Messages
    m_ws_broadcaster->registerHintCallback(boost::bind(&SVHFingerManager::receivedHintMessage,this,_1));

    m_ws_broadcaster->robot->setInputToRadFactor(1);
    m_ws_broadcaster->robot->setHint(eHT_NOT_CONNECTED);
    m_ws_broadcaster->sendHints(); // Hints are updated Manually
    m_ws_broadcaster->sendState(); // Initial send in case someone is waiting for it
  }
#endif


  // load home position default parameters
  setDefaultHomeSettings();

  // set default reset order of all channels
  m_reset_order.resize(eSVH_DIMENSION);
  m_reset_order[0] = eSVH_INDEX_FINGER_PROXIMAL;
  m_reset_order[1] = eSVH_MIDDLE_FINGER_PROXIMAL;
  m_reset_order[2] = eSVH_THUMB_OPPOSITION;
  m_reset_order[3] = eSVH_THUMB_FLEXION;
  m_reset_order[4] = eSVH_FINGER_SPREAD;
  m_reset_order[5] = eSVH_MIDDLE_FINGER_DISTAL;
  m_reset_order[6] = eSVH_INDEX_FINGER_DISTAL;
  m_reset_order[7] = eSVH_RING_FINGER;
  m_reset_order[8] = eSVH_PINKY;

  for (size_t i = 0; i < eSVH_DIMENSION; ++i)
  {
    m_is_switched_off[i] = disable_mask[i];
    if (m_is_switched_off[i])
    {
      LOGGING_INFO_C(DriverSVH, SVHFingerManager, "Joint: " << m_controller->m_channel_description[i] << " was disabled as per user request. It will not do anything!" << endl);
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
      if (m_ws_broadcaster)
      {
        m_ws_broadcaster->robot->setHint(eHT_CHANNEL_SWITCHED_OF);
        m_ws_broadcaster->sendHints(); // Hints are updated Manually
      }
#endif
    }
  }

  m_diagnostic_encoder_state.resize(eSVH_DIMENSION, false);
  m_diagnostic_current_state.resize(eSVH_DIMENSION, false);
  m_diagnostic_current_maximum.resize(eSVH_DIMENSION, 0.0);
  m_diagnostic_current_minimum.resize(eSVH_DIMENSION, 0.0);
  m_diagnostic_position_maximum.resize(eSVH_DIMENSION, 0.0);
  m_diagnostic_position_minimum.resize(eSVH_DIMENSION, 0.0);
  m_diagnostic_deadlock.resize(eSVH_DIMENSION, 0.0);
}

SVHFingerManager::~SVHFingerManager()
{
  if (m_connected)
  {
    disconnect();
  }

  if (m_controller != NULL)
  {
    delete m_controller;
    m_controller = NULL;
  }
}

bool SVHFingerManager::connect(const std::string &dev_name,const unsigned int &_retry_count)
{
  LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Finger manager is trying to connect to the Hardware..." << endl);

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
  // Reset the connection specific hints and give it a go again.
  if (m_ws_broadcaster)
  {
    m_ws_broadcaster->robot->clearHint(eHT_NOT_CONNECTED);
    m_ws_broadcaster->robot->clearHint(eHT_DEVICE_NOT_FOUND);
    m_ws_broadcaster->robot->clearHint(eHT_CONNECTION_FAILED);
    m_ws_broadcaster->sendHints(); // Hints are updated Manually
  }
#endif

  // Save device handle for next use
  m_serial_device = dev_name;


  if (m_connected)
  {
    disconnect();
  }

  if (m_controller != NULL)
  {
    if (m_controller->connect(dev_name))
    {
      unsigned int retry_count=_retry_count;
      do {
        // Reset the package counts (in case a previous attempt was made)
        m_controller->resetPackageCounts();

        // load default position settings before the fingers are resetted
        std::vector<SVHPositionSettings> position_settings = getDefaultPositionSettings(true);

        // load default current settings
        std::vector<SVHCurrentSettings> current_settings
            = getDefaultCurrentSettings();

        m_controller->disableChannel(eSVH_ALL);

        // initialize all channels
        for (size_t i = 0; i < eSVH_DIMENSION; ++i)
        {
          // request controller feedback to have a valid starting point
          m_controller->requestControllerFeedback(static_cast<SVHChannel>(i));

          // Actually set the new position settings
          m_controller->setPositionSettings(static_cast<SVHChannel>(i), position_settings[i]);

          // set current settings
          m_controller->setCurrentSettings(static_cast<SVHChannel>(i), current_settings[i]);
        }

        // check for correct response from hardware controller
        icl_core::TimeStamp start_time = icl_core::TimeStamp::now();
        bool timeout = false;
        unsigned int received_count = 0;
        unsigned int send_count = 0;
        while (!timeout && !m_connected)
        {
          send_count = m_controller->getSentPackageCount();
          received_count = m_controller->getReceivedPackageCount();
          if (send_count == received_count)
          {
            m_connected = true;
            LOGGING_INFO_C(DriverSVH, SVHFingerManager, "Successfully established connection to SCHUNK five finger hand." << endl
                           << "Send packages = " << send_count << ", received packages = " << received_count << endl);

          }
          LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Try to connect to SCHUNK five finger hand: Send packages = " << send_count << ", received packages = " << received_count << endl);

          // check for timeout
          if ((icl_core::TimeStamp::now() - start_time).tsSec() > m_reset_timeout)
          {
            timeout = true;
            LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Connection timeout! Could not connect to SCHUNK five finger hand." << endl
                            << "Send packages = " << send_count << ", received packages = " << received_count << endl);
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
            if (m_ws_broadcaster)
            {
              m_ws_broadcaster->robot->setHint(eHT_CONNECTION_FAILED);
              m_ws_broadcaster->sendHints(); //Hints are updated Manually
            }
#endif

          }
          icl_core::os::usleep(50000);
        }

        // Try again, but ONLY if we at least got one package back, otherwise its futil
        if (!m_connected)
        {
          if (received_count > 0 && retry_count >= 0)
          {
            retry_count--;
            LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Connection Failed! Send packages = " << send_count << ", received packages = " << received_count << ". Retrying, count: " << retry_count << endl);
          }
          else
          {
            retry_count = 0;
            LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Connection Failed! Send packages = " << send_count << ", received packages = " << received_count << ". Not Retrying anymore."<< endl);
          }
        }
        // Keep trying to reconnect several times because the brainbox often makes problems
      } while (!m_connected && retry_count > 0);


      if (!m_connected && retry_count<= 0)
      {
        LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "A Stable connection could NOT be made, however some packages where received. Please check the hardware!" << endl);
      }


      if (m_connected)
      {

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_

        if (m_ws_broadcaster)
        {
          // Intitial connection, any failures regarding the connection must be gone so we can safely clear them all
          m_ws_broadcaster->robot->clearHint(eHT_CONNECTION_FAILED);
          m_ws_broadcaster->robot->clearHint(eHT_NOT_CONNECTED);
          m_ws_broadcaster->robot->clearHint(eHT_DEVICE_NOT_FOUND);
          // Next up, resetting, so give a hint for that
          m_ws_broadcaster->robot->setHint(eHT_NOT_RESETTED);
          m_ws_broadcaster->sendHints(); // Needs to be called if not done by the feedback polling thread
        }
#endif

        // Request firmware information once at the beginning, it will print out on the console
        m_controller->requestFirmwareInfo();
        
        // initialize feedback polling thread
        m_feedback_thread = new SVHFeedbackPollingThread(icl_core::TimeSpan::createFromMSec(100), this);

        // start feedback polling thread
        LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Finger manager is starting the fedback polling thread" << endl);
        if (m_feedback_thread != NULL)
        {
          m_feedback_thread->start();
        }
      }
      else
      {
        //connection open but not stable: close serial port for better reconnect later
        m_controller->disconnect();
      }
    }
    else
    {
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
      if (m_ws_broadcaster)
      {
        m_ws_broadcaster->robot->setHint(eHT_DEVICE_NOT_FOUND);
        m_ws_broadcaster->sendHints(); // Hints are updated Manually
      }
#endif
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Connection FAILED! Device could NOT be opened" << endl);
    }
  }

  return m_connected;
}

void SVHFingerManager::disconnect()
{
  LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Finger manager is trying to discoconnect to the Hardware..." << endl);
  m_connected = false;
  m_connection_feedback_given = false;

  // Disable Polling
  if (m_feedback_thread != NULL)
  {
    // wait until thread has stopped
    m_feedback_thread->stop();
    m_feedback_thread->join();

    delete m_feedback_thread;
    m_feedback_thread = NULL;
    LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Feedback thread terminated" << endl);
  }

  // Tell the Controller to terminate the rest
  if (m_controller != NULL)
  {
    m_controller->disconnect();
  }

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
    // Connection hint is always true when no connection is established :)
    if (m_ws_broadcaster)
    {
      m_ws_broadcaster->robot->clearAllHints();
      m_ws_broadcaster->robot->setHint(eHT_NOT_CONNECTED);
      m_ws_broadcaster->sendHints(); // Hints are Transmitted Manually
    }
#endif

}

//! reset function for a single finger
bool SVHFingerManager::resetChannel(const SVHChannel &channel)
{
  if (m_connected)
  {
    // reset all channels
    if (channel == eSVH_ALL)
    {

      bool reset_all_success = true;
      for (size_t i = 0; i < eSVH_DIMENSION; ++i)
      {
        // try three times to reset each finger
        size_t max_reset_counter = 3;
        bool reset_success = false;
        while (!reset_success && max_reset_counter > 0)
        {
          SVHChannel channel = static_cast<SVHChannel>(m_reset_order[i]);
          reset_success = resetChannel(channel);
          max_reset_counter--;
        }

        LOGGING_DEBUG_C(DriverSVH, resetChannel, "Channel " << m_reset_order[i] << " reset success = " << reset_success << endl);

        // set all reset flag
        reset_all_success = reset_all_success && reset_success;
      }


#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
        // In case we still told the user that this was an issue, it is clearly resolved now.
        if (reset_all_success && m_ws_broadcaster)
        {
          m_ws_broadcaster->robot->clearHint(eHT_RESET_FAILED);
          m_ws_broadcaster->robot->clearHint(eHT_NOT_RESETTED);
          m_ws_broadcaster->sendHints(); // Hints are Transmitted Manually
        }
#endif

      return reset_all_success;
    }
    else if (channel > eSVH_ALL && eSVH_ALL < eSVH_DIMENSION)
    {
      m_diagnostic_encoder_state[channel] = false;
      m_diagnostic_current_state[channel] = false;
      // Tell the websockets
      MovementState last_movement_state = m_movement_state;
      setMovementState(eST_RESETTING);

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
      if (m_ws_broadcaster)
      {
        m_ws_broadcaster->robot->setJointEnabled(false,channel);
        m_ws_broadcaster->robot->setJointHomed(false,channel);
      }
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_


      LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Start homing channel " << channel << endl);
    
      if (!m_is_switched_off[channel])
      {
        LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Setting reset position values for controller of channel " << channel << endl);

        m_controller->setPositionSettings(channel, getDefaultPositionSettings(true)[channel]);

        // reset homed flag
        m_is_homed[channel] = false;

        // read default home settings for channel
        SVHHomeSettings home = m_home_settings[channel];

        SVHPositionSettings pos_set;
        SVHCurrentSettings cur_set;
        m_controller->getPositionSettings(channel, pos_set);
        m_controller->getCurrentSettings(channel, cur_set);

        // find home position
        int32_t position = 0;

        if (home.direction > 0)
        {
          position = static_cast<int32_t>(pos_set.wmx);
        }
        else
        {
          position = static_cast<int32_t>(pos_set.wmn);
        }

        LOGGING_INFO_C(DriverSVH, SVHFingerManager, "Driving channel " << channel << " to hardstop. Detection thresholds: Current MIN: "<< home.resetCurrentFactor * cur_set.wmn << "mA MAX: "<< home.resetCurrentFactor * cur_set.wmx <<"mA" << endl);

        m_controller->setControllerTarget(channel, position);
        m_controller->enableChannel(channel);

        SVHControllerFeedback control_feedback_previous;
        SVHControllerFeedback control_feedback;

        // initialize timeout
        icl_core::TimeStamp start_time = icl_core::TimeStamp::now();
        icl_core::TimeStamp start_time_log = icl_core::TimeStamp::now();
        // Debug helper to just notify about fresh stales
        bool stale_notification_sent = false;

        for (size_t hit_count = 0; hit_count < 10; )
        {
          m_controller->setControllerTarget(channel, position);
          //m_controller->requestControllerFeedback(channel);
          m_controller->getControllerFeedback(channel, control_feedback);
          // Timeout while no encoder ticks changed

          // Quite extensive Current output!
          if ((icl_core::TimeStamp::now() - start_time_log).milliSeconds() > 250)
          {
            LOGGING_INFO_C(DriverSVH, SVHFingerManager,"Resetting Channel "<< channel << ":" << m_controller->m_channel_description[channel] << " current: " << control_feedback.current << " mA" << endl);
            start_time_log = icl_core::TimeStamp::now();
          }

          double threshold = 80;
          // have a look for deadlocks
          if (home.direction == +1)
          {
            double delta = control_feedback.current - m_diagnostic_current_maximum[channel]; // without deadlocks delta should be positiv
            if (delta <= -threshold)
            {
              if (abs(delta) > m_diagnostic_deadlock[channel])
              {
                m_diagnostic_deadlock[channel] = abs(delta);
              }
            }
          }
          else
          {
            double delta = control_feedback.current - m_diagnostic_current_minimum[channel];
            if (delta >= threshold)
            {
              if (abs(delta) > m_diagnostic_deadlock[channel])
              {
                m_diagnostic_deadlock[channel] = abs(delta);
              }
            }
          }

          // save the maximal/minimal current of the motor
          if (control_feedback.current > m_diagnostic_current_maximum[channel])
          {
            m_diagnostic_current_maximum[channel] = control_feedback.current;
          }
          else
          {
            if(control_feedback.current < m_diagnostic_current_minimum[channel])
            {
              m_diagnostic_current_minimum[channel] = control_feedback.current;
            }
          }

          if ((home.resetCurrentFactor * cur_set.wmn >= control_feedback.current) || (control_feedback.current >= home.resetCurrentFactor * cur_set.wmx))
          {
            m_diagnostic_current_state[channel] = true; // when in maximum the current controller is ok

            hit_count++;
            LOGGING_TRACE_C(DriverSVH, SVHFingerManager,"Resetting Channel "<< channel << ":" << m_controller->m_channel_description[channel] << " Hit Count increased: " << hit_count << endl);
          }
          else if (hit_count > 0)
          {
            hit_count--;
            LOGGING_TRACE_C(DriverSVH, SVHFingerManager,"Resetting Channel "<< channel << ":" << m_controller->m_channel_description[channel] << " Hit Count Decreased: " << hit_count << endl);
          }

          // check for time out: Abort, if position does not change after homing timeout.
          if ((icl_core::TimeStamp::now() - start_time).tsSec() > m_homing_timeout)
          {
            m_controller->disableChannel(eSVH_ALL);
            LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Timeout: Aborted finding home position for channel " << channel << endl);
            // Timeout could mean serious hardware issues or just plain wrong settings
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_

            if (m_ws_broadcaster)
            {
              m_ws_broadcaster->robot->setHint(eHT_RESET_FAILED);
              m_ws_broadcaster->sendHints(); // Hints are Transmitted Manually
            }
#endif
            return false;
          }

          // reset time if position changes
          if (control_feedback.position != control_feedback_previous.position)
          {
            m_diagnostic_encoder_state[channel] = true;
            // save the maximal/minimal position the channel can reach
            if (control_feedback.position > m_diagnostic_position_maximum[channel])
              m_diagnostic_position_maximum[channel] = control_feedback.position;
            else
              if(control_feedback.position < m_diagnostic_position_minimum[channel])
                m_diagnostic_position_minimum[channel] = control_feedback.position;

            start_time = icl_core::TimeStamp::now();
            if (stale_notification_sent)
            {
              LOGGING_TRACE_C(DriverSVH, SVHFingerManager,"Resetting Channel "<< channel << ":" << m_controller->m_channel_description[channel] << " Stale resolved, continuing detection" << endl);
              stale_notification_sent = false;
            }
          }
          else
          {
            if (!stale_notification_sent)
            {
              LOGGING_TRACE_C(DriverSVH, SVHFingerManager,"Resetting Channel "<< channel << ":" << m_controller->m_channel_description[channel] << " Stale detected. Starting Timeout" << endl);
              stale_notification_sent = true;
            }
          }

          // save previous control feedback
          control_feedback_previous = control_feedback;
          //icl_core::os::usleep(8000);
        }

        LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Hit counter of " << channel << " reached." << endl);


        // set reference values
        m_position_min[channel] = static_cast<int32_t>(control_feedback.position + std::min(home.minimumOffset, home.maximumOffset));
        m_position_max[channel] = static_cast<int32_t>(control_feedback.position + std::max(home.minimumOffset, home.maximumOffset));
        m_position_home[channel] = static_cast<int32_t>(control_feedback.position + home.idlePosition);
        LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Setting soft stops for Channel " << channel << " min pos = " << m_position_min[channel]
                        << " max pos = " << m_position_max[channel] << " home pos = " << m_position_home[channel] << endl);

        // position will now be reached to release the motor and go into soft stops
        position = m_position_home[channel];

        // go to idle position
        // use the declared start_time variable for the homing timeout
        start_time = icl_core::TimeStamp::now();
        while (true)
        {
          m_controller->setControllerTarget(channel, position);
          //m_controller->requestControllerFeedback(channel);
          m_controller->getControllerFeedback(channel, control_feedback);

          LOGGING_TRACE_C(DriverSVH, SVHFingerManager,"Homing Channel "<< channel << ":" << m_controller->m_channel_description[channel] << " current: " << control_feedback.current << " mA, position ticks: " << control_feedback.position <<  endl);

          if (abs(position - control_feedback.position) < 1000)
          {
            m_is_homed[channel] = true;
            break;
          }

          // if the finger hasn't reached the home position after m_homing_timeout there is an hardware error
          if((icl_core::TimeStamp::now() - start_time).tsSec() > m_homing_timeout)
          {
            m_is_homed[channel] = false;
            LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Channel " << channel << " home position is not reachable after " << m_homing_timeout << "s! There could be an hardware error!" << endl);
            break;
          }
        }

        m_controller->disableChannel(eSVH_ALL);
        //icl_core::os::usleep(8000);
        LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Restoring default position values for controller of channel " << channel << endl);
        m_controller->setPositionSettings(channel, getDefaultPositionSettings(false)[channel]);
      }
      else
      {
        LOGGING_INFO_C(DriverSVH, SVHFingerManager, "Channel " << channel << "switched of by user, homing is set to finished" << endl);
        m_is_homed[channel] = true;
      }

      // Check if this reset has trigger the reset of all the Fingers
      bool reset_all_success = true;
      for (size_t i = 0; i < eSVH_DIMENSION; ++i)
      {
        reset_all_success == reset_all_success && m_is_homed[channel];
      }

      if (reset_all_success)
      {
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
        // In case we still told the user that this was an issue, it is clearly resolved now.
        if (m_ws_broadcaster)
        {
          m_ws_broadcaster->robot->clearHint(eHT_RESET_FAILED);
          m_ws_broadcaster->robot->clearHint(eHT_NOT_RESETTED);
          m_ws_broadcaster->sendHints(); // Hints are Transmitted Manually
        }
#endif
        setMovementState(eST_RESETTED);
      }
      else
      {
        setMovementState(last_movement_state);
      }

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
      if (m_ws_broadcaster)
      {
        m_ws_broadcaster->robot->setJointHomed(true,channel);
      }
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_

      LOGGING_INFO_C(DriverSVH, SVHFingerManager, "Successfully homed channel " << channel << endl);

      return true;
    }
    else
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Channel " << channel << " is out of bounds!" << endl);
      return false;
    }
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not reset channel " << channel << ": No connection to SCHUNK five finger hand!" << endl);
    return false;
  }
}

bool SVHFingerManager::getDiagnosticStatus(const SVHChannel &channel, struct diagnostic_state &diagnostic_status)
{
  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    diagnostic_status.diagnostic_encoder_state = m_diagnostic_encoder_state[channel];
    diagnostic_status.diagnostic_motor_state = m_diagnostic_current_state[channel];
    diagnostic_status.diagnostic_current_maximum = m_diagnostic_current_maximum[channel];
    diagnostic_status.diagnostic_current_minimum = m_diagnostic_current_minimum[channel];
    diagnostic_status.diagnostic_position_maximum = m_diagnostic_position_maximum[channel];
    diagnostic_status.diagnostic_position_minimum = m_diagnostic_position_minimum[channel];
    diagnostic_status.diagnostic_deadlock = m_diagnostic_deadlock[channel];
    return true;
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not get diagnostic status for unknown/unsupported channel " << channel << endl);
    return false;
  }
}


// enables controller of channel
bool SVHFingerManager::enableChannel(const SVHChannel &channel)
{
  if (isConnected() && isHomed(channel))
  {
    if (channel == eSVH_ALL)
    {
      for (size_t i = 0; i < eSVH_DIMENSION; ++i)
      {
        // Just for safety, enable channels in the same order as we have resetted them (otherwise developers might geht confused)
        SVHChannel real_channel = static_cast<SVHChannel>(m_reset_order[i]);
        if (!m_is_switched_off[real_channel])
        {
          // recursion to get the other updates corresponing with activation of a channel
          enableChannel(real_channel);
        }
      }
    }
    else if (channel > eSVH_ALL && eSVH_ALL < eSVH_DIMENSION)
    {
      // Note: This part is another one of these places where the names can lead to confusion. I am sorry about that
      // Switched off is a logical term. The user has chosen NOT to use this channel because of hardware trouble.
      // To enable a smooth driver behaviour all replys regarding these channels will be answered in the most positive way
      // the caller could expect. Enabled refers to the actual enabled state of the hardware controller loops that drive the motors.
      // As the user has chosen not to use certain channels we explicitly do NOT enable these but tell a calling driver that we did
      if (!m_is_switched_off[channel])
      {
        m_controller->enableChannel(channel);
      }

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
      if (m_ws_broadcaster)
      {
        m_ws_broadcaster->robot->setJointEnabled(true,channel);
      }
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_

      setMovementState(eST_PARTIALLY_ENABLED);
      if (isEnabled(eSVH_ALL))
      {
        setMovementState(eST_ENABLED);
      }
    }
    return true;
  }
  return false;
}

void SVHFingerManager::disableChannel(const SVHChannel &channel)
{
  if (channel == eSVH_ALL)
  {
    for (size_t i = 0; i < eSVH_DIMENSION; ++i)
    {
      disableChannel(static_cast<SVHChannel>(i));
    }
  }
  else
  {
    if (!m_is_switched_off[channel])
    {
      m_controller->disableChannel(channel);
    }

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
    if (m_ws_broadcaster)
    {
      m_ws_broadcaster->robot->setJointEnabled(false,channel);
    }
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_

    setMovementState(eST_PARTIALLY_ENABLED);

    bool all_disabled = true;
    for (size_t i = 0; i < eSVH_DIMENSION; ++i)
    {
      // Again only check channels that are not switched off. Switched off channels will always answer that they are enabled
      all_disabled = all_disabled && (m_is_switched_off[channel] ||!isEnabled(static_cast<SVHChannel>(i)));
    }
    if (all_disabled)
    {
      setMovementState(eST_DEACTIVATED);
    }

  }
}

bool SVHFingerManager::requestControllerFeedback(const SVHChannel &channel)
{
  if (isConnected())
  {
    m_controller->requestControllerFeedback(channel);
    return true;
  }

  LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Feedback for channel " << channel << " could not be requested. FM is not connected to HW." << endl);
  return false;
}

// returns actual position value for given channel
bool SVHFingerManager::getPosition(const SVHChannel &channel, double &position)
{
  SVHControllerFeedback controller_feedback;
  if ((channel >=0 && channel < eSVH_DIMENSION) && isHomed(channel) && m_controller->getControllerFeedback(channel, controller_feedback))
  {
    // Switched off channels will always remain at zero position as the tics we get back migh be total gibberish
    if (m_is_switched_off[channel])
    {
      position = 0.0;
      return true;
    }

    //int32_t cleared_position_ticks = controller_feedback.position;
    position = convertTicks2Rad(channel,controller_feedback.position);

    // Safety overwrite: If controller drives to a negative position (should not happen but might in case the soft stops are placed badly)
    // we cannot get out because inputs smaller than 0 will be ignored
    if (position < 0)
    {
      position = 0.0;
    }

    // DISABLED as the output was realy spamming everything else :)
    //LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Channel " << channel << ": position_ticks = " << controller_feedback.position
    //                << " | cleared_position_ticks = " << cleared_position_ticks << " | position rad = " << position << endl);
    return true;
  }
  else
  {
    LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Could not get postion for channel " << channel << endl);
    return false;
  }
}


#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
void SVHFingerManager::receivedHintMessage(const int &hint)
{
  LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Received a special command to clear error :" << hint << endl);
  switch (hint)
  {
  case eHT_DEVICE_NOT_FOUND:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Retrying connection with device handle: " << m_serial_device << endl);
    connect(m_serial_device);
    break;
  case eHT_CONNECTION_FAILED:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Retrying connection with device handle: " << m_serial_device << endl);
    connect(m_serial_device);
    break;
  case eHT_NOT_RESETTED:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Resetting ALL fingers " << endl);
    resetChannel(eSVH_ALL);
    break;
  case eHT_NOT_CONNECTED:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Retrying connection with device handle: " << m_serial_device << endl);
    connect(m_serial_device);
    break;
  case eHT_RESET_FAILED:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Resetting ALL fingers " << endl);
    resetChannel(eSVH_ALL);
    break;
  case eHT_CHANNEL_SWITCHED_OF:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "No specific action associated with command" << hint << endl);
    break;
  case eHT_DANGEROUS_CURRENTS:
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "No specific action associated with command" << hint << endl);
    break;
  default:
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Special error clearing command " << hint << " could not be mapped. No action is taken please contact support if this happens." << endl);
    break;
  }
}
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
void SVHFingerManager::updateWebSocket()
{
  if (m_ws_broadcaster)
  {
    double position;
    //double current // will be implemented in future releases
    for (size_t i = 0; i < eSVH_DIMENSION; ++i)
    {
      // NOTE: Although the call to getPosition and current cann fail due to multiple reason, the only one we would encounter with these calls is a
      // non-homed finger. So it is quite safe to assume that the finger is NOT homed if these calls fail and we can do without multiple acces to the homed variable

      if (isHomed(static_cast<SVHChannel>(i)) && getPosition(static_cast<SVHChannel>(i),position)) // && (getCurrent(i,current))
      {
        m_ws_broadcaster->robot->setJointPosition(position,i);
        //m_ws_broadcaster>robot>setJointCurrent(current,i); // will be implemented in future releases
      }
      else
      {
        m_ws_broadcaster->robot->setJointHomed(false,i);
      }

      // One of the few places we actually need to call the sendstate as this function is periodically called by the feedback polling thread
      if (!m_ws_broadcaster->sendState())
      {
        //LOGGING_INFO_C(DriverSVH, SVHFingerManager, "Can't send ws_broadcaster state - reconnect pending..." << endl);
      }
    }
  }
}
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_





// returns actual current value for given channel
bool SVHFingerManager::getCurrent(const SVHChannel &channel, double &current)
{
  SVHControllerFeedback controller_feedback;
  if ((channel >=0 && channel < eSVH_DIMENSION) && isHomed(channel) && m_controller->getControllerFeedback(channel, controller_feedback))
  {
    current = controller_feedback.current;
    return true;
  }
  else
  {
    LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Could not get current for channel " << channel << endl);
    return false;
  }
}

// set all target positions at once
bool SVHFingerManager::setAllTargetPositions(const std::vector<double>& positions)
{
  if (isConnected())
  {
    // check size of position vector
    if (positions.size() == eSVH_DIMENSION)
    {
      // create target positions vector
      std::vector<int32_t> target_positions(eSVH_DIMENSION, 0);

      bool reject_command = false;
      for (size_t i = 0; i < eSVH_DIMENSION; ++i)
      {
        SVHChannel channel = static_cast<SVHChannel>(i);

        // enable all homed and disabled channels.. except its switched of
        if (!m_is_switched_off[channel] && isHomed(channel) && !isEnabled(channel))
        {
          enableChannel(channel);
        }

        // convert all channels to ticks
        target_positions[channel] = convertRad2Ticks(channel, positions[channel]);

        // check for out of bounds (except the switched off channels)
        if (!m_is_switched_off[channel] && !isInsideBounds(channel, target_positions[channel]))
        {
          reject_command = true;

        }
      }

      // send target position vector to controller and SCHUNK hand
      if (!reject_command)
      {
        m_controller->setControllerTargetAllChannels(target_positions);
        return true;
      }
      else
      {
        LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Could not set target position vector: At least one channel is out of bounds!" << endl);
        return false;
      }

    }
    else
    {
      LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Size of target position vector wrong: size = " << positions.size() << " expected size = " << (int)eSVH_DIMENSION << endl);
      return false;
    }
  }
  else
  {
    if (!m_connection_feedback_given)
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set target position vector: No connection to SCHUNK five finger hand!" << endl);
      m_connection_feedback_given = true;
    }
    return false;
  }
}

bool SVHFingerManager::setTargetPosition(const SVHChannel &channel, double position, double current)
{
  if (isConnected())
  {
    if (channel >= 0 && channel < eSVH_DIMENSION)
    {
      if (m_is_switched_off[channel])
      {
        // Switched off channels  behave transparent so we return a true value while we ignore the input
        LOGGING_TRACE_C(DriverSVH, SVHFingerManager, "Target position for channel " << channel << " was ignored as it is switched off by the user"<< endl);
        return true;
      }


      if (isHomed(channel))
      {
        int32_t target_position = convertRad2Ticks(channel, position);

        //Disabled as the output will spam everything
        //LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Target position for channel " << channel << " = " << target_position << endl);

        // check for bounds
        if (isInsideBounds(channel, target_position))
        {
          if (!isEnabled(channel))
          {
            enableChannel(channel);
          }

          m_controller->setControllerTarget(channel, target_position);
          return true;
        }
        else
        {
          LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Target position for channel " << channel << " out of bounds!" << endl);
          return false;
        }
      }
      else
      {
        LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set target position for channel " << channel << ": Reset first!" << endl);
        return false;
      }
    }
    else
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set target position for channel " << channel << ": Illegal Channel" << endl);
      return false;
    }
  }
  else
  {
    // Give the Warning about no Connection exactly once! Otherwise this will immediately spam the log
    if (!m_connection_feedback_given)
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set target position for channel " << channel << ": No connection to SCHUNK five finger hand!" << endl);
      m_connection_feedback_given = true;
    }
    return false;
  }
}

// return enable flag
bool SVHFingerManager::isEnabled(const SVHChannel &channel)
{
  if (channel==eSVH_ALL)
  {
    bool all_enabled = true;
    for (size_t i = 0; i < eSVH_DIMENSION; ++i)
    {
      all_enabled = all_enabled && isEnabled(static_cast<SVHChannel>(i));
      // disabled for now, to noisy
//      if (!isEnabled(static_cast<SVHChannel>(i)))
//      {
//        LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "All finger enabled check failed: Channel: " << channel << " : " << SVHController::m_channel_description[i] << " is not enabled" << endl);
//      }
    }

    return all_enabled;
  }
  else if (channel >=0 && channel < eSVH_DIMENSION)
  {
    // Switched off Channels will aways be reported as enabled to simulate everything is fine. Others need to ask the controller
    // if the channel is realy switched on
    // Note: i can see that based on the names this might lead to a little confusion... sorry about that but there are only limited number of
    // words for not active ;) enabled refers to the actual state of the position and current controllers. So enabled
    // means enabled on a hardware level. Switched off is a logical decission in this case. The user has specified this
    // particular channel not to be used (due to hardware issues) and therefore the driver (aka the finger manager) will act
    // AS IF the channel was enabled but is in fact switched off by the user. If you have a better variable name or a better
    // idea how to handle that you are welcome to change it. (GH 2014-05-26)
    return (m_is_switched_off[channel] || m_controller->isEnabled(channel));
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "isEnabled was requested for UNKNOWN Channel: " << channel << endl);
    return false;
  }
}

bool SVHFingerManager::isHomed(const SVHChannel &channel)
{
  if (channel == eSVH_ALL)
  {
    bool all_homed = true;
    for (size_t i = 0; i < eSVH_DIMENSION; ++i)
    {
      all_homed = all_homed && isHomed(static_cast<SVHChannel>(i));
      if (!isHomed(static_cast<SVHChannel>(i)))
      {
        LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "All finger homed check failed: Channel: " << i << " : " << SVHController::m_channel_description[i] << " is not homed" << endl);
      }
    }

    return all_homed;
  }
  else if (channel >=0 && channel < eSVH_DIMENSION)
  {
    // Channels that are switched off will always be reported as homed to simulate everything is fine. Others have to check
    return (m_is_switched_off[channel] || m_is_homed[channel]);
  }
  else //should not happen but better be save than sorry
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "isHomed was requested for UNKNOWN Channel: " << channel << endl);
    return false;
  }
}

void SVHFingerManager::setMovementState(const SVHFingerManager::MovementState &state)
{
  m_movement_state = state;

#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
  if (m_ws_broadcaster)
  {
    m_ws_broadcaster->robot->setMovementState(state);
  }
#endif // _IC_BUILDER_ICL_COMM_WEBSOCKET_
}

bool SVHFingerManager::getCurrentSettings(const SVHChannel &channel, SVHCurrentSettings &current_settings)
{
  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    return m_controller->getCurrentSettings(channel, current_settings);
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not get current settings for unknown/unsupported channel " << channel << endl);
    return false;
  }
}

bool SVHFingerManager::getPositionSettings(const SVHChannel &channel, SVHPositionSettings &position_settings)
{
  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    return m_controller->getPositionSettings(channel, position_settings);
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not get position settings for unknown/unsupported channel " << channel << endl);
    return false;
  }
}

bool SVHFingerManager::getHomeSettings(const SVHChannel &channel, SVHHomeSettings &home_settings)
{
  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    home_settings = m_home_settings[channel];
    return true;
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not get home settings for unknown/unsupported channel " << channel << endl);
    return false;
  }
}

bool SVHFingerManager::currentSettingsAreSafe(const SVHChannel &channel,const SVHCurrentSettings &current_settings)
{
  bool settingsAreSafe = false;

  if(!isEnabled(eSVH_ALL))
  {
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Fingers are not all enabled -> no safety tests" << endl);
    // befor the fingers are homed no finger-data are valid
    return true;
  }

  if (current_settings.wmx <= m_max_current_percentage *
      std::max(m_diagnostic_current_maximum[channel], std::abs(m_diagnostic_position_minimum[channel])))
  {
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager, "Current settings are safe!" << endl);
    settingsAreSafe = true;
  }
  else
  {
    LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Current value given: "
            <<  current_settings.wmx << " is not valid." << endl);
    LOGGING_DEBUG_C(DriverSVH, SVHFingerManager," Please provide values between "
            << " 0 - " << m_max_current_percentage * std::max(m_diagnostic_current_maximum[channel], std::abs(m_diagnostic_position_minimum[channel]))
            << " [mA] or 0 - " << convertmAtoN(channel, m_max_current_percentage * std::max(m_diagnostic_current_maximum[channel], std::abs(m_diagnostic_position_minimum[channel])))
            << " [N]" << endl);
  }

  return settingsAreSafe;
}

// overwrite current parameters
bool SVHFingerManager::setCurrentSettings(const SVHChannel &channel, const SVHCurrentSettings &current_settings)
{

  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    // For now we will prefent current settings with more current than possible
    if (!currentSettingsAreSafe(channel,current_settings))
    {
      // LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "WARNING!!! Current Controller Params for channel " << channel << " are dangerous! THIS MIGHT DAMAGE YOUR HARDWARE!!!" << endl);
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "WARNING!!! Current Controller Params for channel " << channel << " would be dangerous! Currents are limited!!!" << endl);
#ifdef _IC_BUILDER_ICL_COMM_WEBSOCKET_
      if (m_ws_broadcaster)
      {
        m_ws_broadcaster->robot->setHint(eHT_DANGEROUS_CURRENTS);
        m_ws_broadcaster->sendHints(); // Hints are Transmitted Manually
      }
#endif
      return false;
    }

    // First of save the values
    m_current_settings[channel] = current_settings;
    m_current_settings_given[channel] = true;

    // In case the Hardware is connected, update the values
    if (isConnected())
    {
        m_controller->setCurrentSettings(channel, current_settings);
    }
    return true;
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set Current Controller Params for channel " << channel << ": No such channel" << endl);
    return false;
  }
}

// overwrite position parameters
bool SVHFingerManager::setPositionSettings(const SVHChannel &channel, const SVHPositionSettings &position_settings)
{

  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    // First of save the values
    m_position_settings[channel] = position_settings;
    m_position_settings_given[channel] = true;

    // In case the Hardware is connected, update the values
    if (isConnected())
    {
      m_controller->setPositionSettings(channel, position_settings);
    }

    return true;
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set Position Controller Params for channel " << channel << ": No such channel" << endl);
    return false;
  }
}

//overwirte home settings
bool SVHFingerManager::setHomeSettings(const SVHChannel &channel, const driver_svh::SVHHomeSettings &home_settings)
{
  if (channel >=0 && channel < eSVH_DIMENSION)
  {
    // First of save the values
    m_home_settings[channel] = home_settings;
    LOGGING_TRACE_C(DriverSVH,SVHFingerManager, "Channel " << channel << " setting new homing settings : ");
    LOGGING_TRACE_C(DriverSVH,SVHFingerManager, "Direction " << home_settings.direction << " " << "Min offset " << home_settings.minimumOffset << " "
                                             << "Max offset "<< home_settings.maximumOffset << " " << "idle pos "  << home_settings.idlePosition  << " "
                                             << "Range Rad " << home_settings.rangeRad << " " << "Reset Curr Factor " << home_settings.resetCurrentFactor << " " << endl
                    );

    // Update the conversion factor for this finger:
    float range_ticks = m_home_settings[channel].maximumOffset - m_home_settings[channel].minimumOffset;
    m_ticks2rad[channel] = m_home_settings[channel].rangeRad / range_ticks * (-m_home_settings[channel].direction);

    return true;
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not set homing settings for channel " << channel << ": No such channel" << endl);
    return false;
  }
}

bool SVHFingerManager::resetDiagnosticData(const SVHChannel &channel)
{
  // reset all channels
  if (channel == eSVH_ALL)
  {
    for(size_t i=0; i<= eSVH_DIMENSION; ++i)
    {
      m_diagnostic_encoder_state[i] = false;
      m_diagnostic_current_state[i] = false;
      m_diagnostic_current_maximum[i] = 0.0;
      m_diagnostic_current_minimum[i] = 0.0;
      m_diagnostic_position_maximum[i] = 0.0;
      m_diagnostic_position_minimum[i] = 0.0;
      m_diagnostic_deadlock[i] = 0.0;
    }
    LOGGING_TRACE_C(DriverSVH,SVHFingerManager, "Diagnostic data for all channel reseted successfully");
    return true;
  }
  else
  {
    if (channel > 0 && channel <= eSVH_DIMENSION)
    {
      m_diagnostic_encoder_state[channel] = false;
      m_diagnostic_current_state[channel] = false;
      m_diagnostic_current_maximum[channel] = 0.0;
      m_diagnostic_current_minimum[channel] = 0.0;
      m_diagnostic_position_maximum[channel] = 0.0;
      m_diagnostic_position_minimum[channel] = 0.0;
      LOGGING_TRACE_C(DriverSVH,SVHFingerManager, "Diagnostic data for channel " << channel << " reseted successfully");
      return true;
    }
    else
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Could not reset diagnostic data for channel " << channel << ": No such channel" << endl);
      return false;
    }
  }
}

void SVHFingerManager::setDefaultHomeSettings()
{
  // homing parameters are important for software end stops

  // All values are based on the hardware description for maximum tics and maximum allowable range of movements
  // direction, minimum offset, maximum offset, idle position, range in rad, resetcurrent(factor)
  m_home_settings[eSVH_THUMB_FLEXION]          =  SVHHomeSettings(+1, -175.0e3f,  -5.0e3f, -15.0e3f, 0.97, 0.75);    // thumb flexion
  // Conservative value
  //m_home_settings[eSVH_THUMB_OPPOSITION]       =  SVHHomeSettings(+1, -105.0e3f,  -5.0e3f, -15.0e3f, 0.99, 0.75); // thumb opposition
  // Value using the complete movemment range
  m_home_settings[eSVH_THUMB_OPPOSITION]       =  SVHHomeSettings(+1, -150.0e3f,  -5.0e3f, -15.0e3f, 0.99, 0.75); // thumb opposition
  m_home_settings[eSVH_INDEX_FINGER_DISTAL]    =  SVHHomeSettings(+1,  -47.0e3f,  -2.0e3f,  -8.0e3f, 1.33, 0.75);    // index finger distal joint
  m_home_settings[eSVH_INDEX_FINGER_PROXIMAL]  =  SVHHomeSettings(-1,    2.0e3f,  42.0e3f,   8.0e3f, 0.8, 0.75);  // index finger proximal joint
  m_home_settings[eSVH_MIDDLE_FINGER_DISTAL]   =  SVHHomeSettings(+1,  -47.0e3f,  -2.0e3f,  -8.0e3f, 1.33, 0.75);    // middle finger distal joint
  m_home_settings[eSVH_MIDDLE_FINGER_PROXIMAL] =  SVHHomeSettings(-1,    2.0e3f,  42.0e3f,   8.0e3f, 0.8, 0.75);  // middle finger proximal joint
  m_home_settings[eSVH_RING_FINGER]            =  SVHHomeSettings(+1,  -47.0e3f,  -2.0e3f,  -8.0e3f, 0.98, 0.75);    // ring finger
  m_home_settings[eSVH_PINKY]                  =  SVHHomeSettings(+1,  -47.0e3f,  -2.0e3f,  -8.0e3f, 0.98, 0.75);    // pinky
  m_home_settings[eSVH_FINGER_SPREAD]          =  SVHHomeSettings(+1,  -47.0e3f,  -2.0e3f,  -25.0e3f,0.58, 0.4);    // finger spread

  m_ticks2rad.resize(eSVH_DIMENSION, 0.0);
  for (size_t i = 0; i < eSVH_DIMENSION; ++i)
  {
    float range_ticks = m_home_settings[i].maximumOffset - m_home_settings[i].minimumOffset;
    m_ticks2rad[i] = m_home_settings[i].rangeRad / range_ticks * (-m_home_settings[i].direction);
  }

}



std::vector<SVHCurrentSettings> SVHFingerManager::getDefaultCurrentSettings()
{
  // BEWARE! Only change these values if you know what you are doing !! Setting wrong values could damage the hardware!!!

  std::vector<SVHCurrentSettings> current_settings(eSVH_DIMENSION);


  // curr min, Curr max,ky(error output scaling),dt(time base),imn (integral windup min), imx (integral windup max), kp,ki,umn,umx (output limter)
  // More accurate values used in the new param files for SVH V1
  SVHCurrentSettings cur_set_thumb(           -500.0f, 500.0f, 0.405f, 4e-6f, -25.0f, 25.0f, 0.6f, 10.0f, -255.0f, 255.0f);
  SVHCurrentSettings cur_set_thumb_opposition(-500.0f, 500.0f, 0.405f, 4e-6f, -25.0f, 25.0f, 1.0f, 10.0f, -255.0f, 255.0f);
  SVHCurrentSettings cur_set_distal_joint(    -300.0f, 300.0f, 0.405f, 4e-6f, -25.0f, 25.0f, 1.0f, 10.0f, -255.0f, 255.0f);
  SVHCurrentSettings cur_set_proximal_joint(  -350.0f, 350.0f, 0.405f, 4e-6f, -25.0f, 25.0f, 1.0f, 10.0f, -255.0f, 255.0f);
  SVHCurrentSettings cur_set_outer_joint(     -300.0f, 300.0f, 0.405f, 4e-6f, -10.0f, 10.0f, 1.0f, 25.0f, -255.0f, 255.0f);
  SVHCurrentSettings cur_set_finger_spread(   -500.0f, 500.0f, 0.405f, 4e-6f,  -4.0f,  4.0f, 0.7f, 60.0f, -255.0f, 255.0f);


  current_settings[eSVH_THUMB_FLEXION]          = m_current_settings_given[eSVH_THUMB_FLEXION]          ? m_current_settings[eSVH_THUMB_FLEXION]          :cur_set_thumb;              // thumb flexion
  current_settings[eSVH_THUMB_OPPOSITION]       = m_current_settings_given[eSVH_THUMB_OPPOSITION]       ? m_current_settings[eSVH_THUMB_OPPOSITION]       :cur_set_thumb_opposition;   // thumb opposition
  current_settings[eSVH_INDEX_FINGER_DISTAL]    = m_current_settings_given[eSVH_INDEX_FINGER_DISTAL]    ? m_current_settings[eSVH_INDEX_FINGER_DISTAL]    :cur_set_distal_joint;       // index finger distal joint
  current_settings[eSVH_INDEX_FINGER_PROXIMAL]  = m_current_settings_given[eSVH_INDEX_FINGER_PROXIMAL]  ? m_current_settings[eSVH_INDEX_FINGER_PROXIMAL]  :cur_set_proximal_joint;     // index finger proximal joint
  current_settings[eSVH_MIDDLE_FINGER_DISTAL]   = m_current_settings_given[eSVH_MIDDLE_FINGER_DISTAL]   ? m_current_settings[eSVH_MIDDLE_FINGER_DISTAL]   :cur_set_distal_joint;       // middle finger distal joint
  current_settings[eSVH_MIDDLE_FINGER_PROXIMAL] = m_current_settings_given[eSVH_MIDDLE_FINGER_PROXIMAL] ? m_current_settings[eSVH_MIDDLE_FINGER_PROXIMAL] :cur_set_proximal_joint;     // middle finger proximal joint
  current_settings[eSVH_RING_FINGER]            = m_current_settings_given[eSVH_RING_FINGER]            ? m_current_settings[eSVH_RING_FINGER]            :cur_set_outer_joint;        // ring finger
  current_settings[eSVH_PINKY]                  = m_current_settings_given[eSVH_PINKY]                  ? m_current_settings[eSVH_PINKY]                  :cur_set_outer_joint;        // pinky
  current_settings[eSVH_FINGER_SPREAD]          = m_current_settings_given[eSVH_FINGER_SPREAD]          ? m_current_settings[eSVH_FINGER_SPREAD]          :cur_set_finger_spread;      // finger spread

  return current_settings;
}

//!
//! \brief returns parameters for position settings either the default ones or parameters that have been set from outside
//!
std::vector<SVHPositionSettings> SVHFingerManager::getDefaultPositionSettings(const bool& reset)
{
  std::vector<SVHPositionSettings> position_settings(eSVH_DIMENSION);

  // Original conservative settings
//  SVHPositionSettings pos_set_thumb = {-1.0e6f, 1.0e6f,  3.4e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f};
//  SVHPositionSettings pos_set_finger = {-1.0e6f, 1.0e6f,  8.5e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f};
//  SVHPositionSettings pos_set_spread = {-1.0e6f, 1.0e6f, 17.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f};

  // All Fingers with a speed that will close the complete range of the finger in 1 Seconds    (except the thumb that will take 4)
  SVHPositionSettings pos_set_thumb_flexion            (-1.0e6f, 1.0e6f,  65.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.0f, 400.0f);
  SVHPositionSettings pos_set_thumb_opposition         (-1.0e6f, 1.0e6f,  50.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.1f, 100.0f);
  SVHPositionSettings pos_set_finger_index_distal      (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.0f, 40.0f);
  SVHPositionSettings pos_set_finger_index_proximal    (-1.0e6f, 1.0e6f,  40.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.8f, 0.0f, 1000.0f);
  SVHPositionSettings pos_set_finger_middle_distal     (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.0f, 10.0f);
  SVHPositionSettings pos_set_finger_middle_proximal   (-1.0e6f, 1.0e6f,  40.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.8f, 0.0f, 1000.0f);
  SVHPositionSettings pos_set_finger_ring              (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.0f, 100.0f);
  SVHPositionSettings pos_set_finger_pinky             (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.0f, 100.0f);
  SVHPositionSettings pos_set_spread                   (-1.0e6f, 1.0e6f,  25.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.0f, 100.0f);

  // OLD (from diagnostics) - All Fingers with a speed that will close the complete range of the finger in 1 Seconds    (except the thumb that wikll take 4)
//  SVHPositionSettings pos_set_thumb_flexion            (-1.0e6f, 1.0e6f,  65.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f);
//  SVHPositionSettings pos_set_thumb_opposition         (-1.0e6f, 1.0e6f,  50.0e3f, 1.00f, 1e-3f, -4000.0f, 4000.0f, 0.05f, 0.1f, 0.0f);
//  SVHPositionSettings pos_set_finger_index_distal      (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f);
//  SVHPositionSettings pos_set_finger_index_proximal    (-1.0e6f, 1.0e6f,  40.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.3f, 0.05f, 0.0f);
//  SVHPositionSettings pos_set_finger_middle_distal     (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f);
//  SVHPositisonSettings pos_set_finger_middle_proximal   (-1.0e6f, 1.0e6f,  40.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.3f, 0.05f, 0.0f);
//  SVHPositionSettings pos_set_finger_ring              (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f);
//  SVHPositionSettings pos_set_finger_pinky             (-1.0e6f, 1.0e6f,  45.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f);
//  SVHPositionSettings pos_set_spread                   (-1.0e6f, 1.0e6f,  25.0e3f, 1.00f, 1e-3f, -500.0f, 500.0f, 0.5f, 0.05f, 0.0f);


  //Return either the default values or the ones given from outside
  position_settings[eSVH_THUMB_FLEXION]           = m_position_settings_given[eSVH_THUMB_FLEXION] ? m_position_settings[eSVH_THUMB_FLEXION] : pos_set_thumb_flexion;   // thumb flexion
  position_settings[eSVH_THUMB_OPPOSITION]        = m_position_settings_given[eSVH_THUMB_OPPOSITION] ? m_position_settings[eSVH_THUMB_OPPOSITION] :pos_set_thumb_opposition;   // thumb opposition
  position_settings[eSVH_INDEX_FINGER_DISTAL]     = m_position_settings_given[eSVH_INDEX_FINGER_DISTAL] ? m_position_settings[eSVH_INDEX_FINGER_DISTAL] :pos_set_finger_index_distal;  // index finger distal joint
  position_settings[eSVH_INDEX_FINGER_PROXIMAL]   = m_position_settings_given[eSVH_INDEX_FINGER_PROXIMAL] ? m_position_settings[eSVH_INDEX_FINGER_PROXIMAL] :pos_set_finger_index_proximal;  // index finger proximal joint
  position_settings[eSVH_MIDDLE_FINGER_DISTAL]    = m_position_settings_given[eSVH_MIDDLE_FINGER_DISTAL] ? m_position_settings[eSVH_MIDDLE_FINGER_DISTAL] :pos_set_finger_middle_distal;  // middle finger distal joint
  position_settings[eSVH_MIDDLE_FINGER_PROXIMAL]  = m_position_settings_given[eSVH_MIDDLE_FINGER_PROXIMAL] ? m_position_settings[eSVH_MIDDLE_FINGER_PROXIMAL] :pos_set_finger_middle_proximal;  // middle finger proximal joint
  position_settings[eSVH_RING_FINGER]             = m_position_settings_given[eSVH_RING_FINGER] ? m_position_settings[eSVH_RING_FINGER] :pos_set_finger_ring;  // ring finger
  position_settings[eSVH_PINKY]                   = m_position_settings_given[eSVH_PINKY] ? m_position_settings[eSVH_PINKY] :pos_set_finger_pinky;  // pinky
  position_settings[eSVH_FINGER_SPREAD]           = m_position_settings_given[eSVH_FINGER_SPREAD]  ? m_position_settings[eSVH_FINGER_SPREAD] :pos_set_spread;  // finger spread

  // Modify the reset speed in case these position settings are meant to be used during the reset
  if (reset)
  {
    for (size_t i = 0; i < eSVH_DIMENSION; ++i)
    {
      position_settings[i].dwmx = position_settings[i].dwmx * m_reset_speed_factor;
    }
  }


  return position_settings;
}

void driver_svh::SVHFingerManager::setResetSpeed(const float &speed)
{
  if ((speed>= 0.0) && (speed <= 1.0))
  {
    m_reset_speed_factor = speed;
  }
  else
  {
    LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "The reset speed value given: "<< speed << " is not valid. Please provide a value between 0.0 and 1.0, default is 0.2"<< endl);
  }
}

// Converts joint positions of a specific channel from RAD to ticks
int32_t SVHFingerManager::convertRad2Ticks(const SVHChannel &channel,const double &position)
{
  int32_t target_position = static_cast<int32_t>(position / m_ticks2rad[channel]);

  if (m_home_settings[channel].direction > 0)
  {
    target_position += m_position_max[channel];
  }
  else
  {
    target_position += m_position_min[channel];
  }

  return target_position;
}

// Converts Joint ticks of a specific channel back to RAD removing its offset in the process
double SVHFingerManager::convertTicks2Rad(const SVHChannel &channel, const int32_t &ticks)
{
    int32_t cleared_position_ticks;

    if (m_home_settings[channel].direction > 0)
    {
      cleared_position_ticks = ticks - m_position_max[channel];
    }
    else
    {
      cleared_position_ticks = ticks - m_position_min[channel];
    }

    return static_cast<double>(cleared_position_ticks * m_ticks2rad[channel]);
}

// Converts joint efforts of a specific channel from force [N] to current [mA]
uint16_t SVHFingerManager::convertNtomA(const SVHChannel &channel, const double &effort)
{
  // percentage effort calculation
  // float target_effort;
  //
  // // persentage
  // if((effort != 0.0) && (effort <= 100.0))
  // {
  //   float norm = m_current_settings[channel].wmx / 100;
  //   //std::cout << "Norm: " << norm << std::endl;
  //   target_effort = static_cast<float>(effort * norm);
  // }
  // else
  // {
  //   target_effort = m_current_settings[channel].wmx;
  // }

  uint16_t current;
  if (SVHController::channel_effort_constants[channel][0] != 0)
  {
    // y = a*x + b -->  x = (y-b) / a
    // y = effort and x = current
    current = static_cast<int>((effort - SVHController::channel_effort_constants[channel][1] ) /
                              SVHController::channel_effort_constants[channel][0] + 0.5);
  }
  else
  {
    current = m_max_current_percentage * m_diagnostic_current_maximum[channel];
  }

  return current;
}

// Converts joint effort of a specific channel from current [mA] to force [N]
double SVHFingerManager::convertmAtoN(const SVHChannel &channel, const int16_t &current)
{
  float effort;
  // y = a*x + b
  // y = effort and x = current
  effort = SVHController::channel_effort_constants[channel][0] * std::abs(current)
          + SVHController::channel_effort_constants[channel][1];

  return effort;
}

// Check bounds of target positions
bool SVHFingerManager::isInsideBounds(const SVHChannel &channel, const int32_t &target_position)
{

  // Switched off channels will always be reported as inside bounds
  if (m_is_switched_off[channel] || ((target_position >= m_position_min[channel]) && (target_position <= m_position_max[channel])))
  {
      return true;
  }
  else
  {
    LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Channel" << channel << " : " << SVHController::m_channel_description[channel]  << " Target: " << target_position << "(" << convertTicks2Rad(channel,target_position) << "rad)" << " is out of bounds! [" << m_position_min[channel] << "/" << m_position_max[channel] << "]"  << endl);
    return false;
  }
}

void SVHFingerManager::requestControllerState()
{
  m_controller->requestControllerState();
}

void SVHFingerManager::setResetTimeout(const int& resetTimeout)
{
  m_reset_timeout = (resetTimeout>0)?resetTimeout:0;
}

bool SVHFingerManager::setMaxForce(float max_force)
{
  if (max_force > 0 && max_force <= 1)
  {
    m_max_current_percentage = max_force;
    return true;
  }
  else
  {
    LOGGING_WARNING_C(DriverSVH, SVHFingerManager, "Maximal Force / current should be in the are of [0,1], was set to: " << max_force << endl);
    return false;
  }
}

float SVHFingerManager::setForceLimit(const SVHChannel &channel, float force_limit)
{
  uint16_t current;
  current = convertNtomA(channel, force_limit);

  SVHCurrentSettings current_settings;

  current_settings = m_current_settings[channel];

  current_settings.wmx = current;
  current_settings.wmn = -current;

  if (setCurrentSettings(channel, current_settings))
  {
    return force_limit;
  }
  else
  {
    return 0.0;
  }
}


SVHFirmwareInfo SVHFingerManager::getFirmwareInfo(const std::string &dev_name,const unsigned int &_retry_count)
{
  bool was_connected = true;
  SVHFirmwareInfo info;
  if (!m_connected)
  {
    was_connected = false;
    if(!m_controller->connect(dev_name))
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Connection FAILED! Device could NOT be opened" << endl);
      info.version_major = 0;
      info.version_minor = 0;
      return info;
    }
  }

  // As the firmware info takes longer we need to disable the polling during the request of the firmware information
  if (m_feedback_thread != NULL)
  {
    // wait until thread has stopped
    m_feedback_thread->stop();
    m_feedback_thread->join();
  }
  
  unsigned int retry_count = _retry_count;
  do
  {
    // Tell the hardware to get the newest firmware information
    m_controller->requestFirmwareInfo();
    // Just wait a tiny amount
    icl_core::os::usleep(100000);
    // Get the Version number if received yet, else 0.0
    info = m_controller->getFirmwareInfo();
    --retry_count;
    if (info.version_major == 0 && info.version_major == 0)
    {
      LOGGING_ERROR_C(DriverSVH, SVHFingerManager, "Getting Firmware Version failed,.Retrying, count: " << retry_count << endl);
    }
  }
  while(retry_count > 0 && info.version_major == 0 && info.version_major == 0);

  // Start the feedback process aggain
  if (m_feedback_thread != NULL && was_connected)
  {
    // wait until thread has stopped
    m_feedback_thread->start();
  }
  
  if (!was_connected)
  {
    m_controller->disconnect();
  }
  // Note that the Firmware will also be printed to the console by the controller. So in case you just want to know it no further action is required
  return info;
}

}
