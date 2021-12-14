// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-


// -- BEGIN LICENSE BLOCK ----------------------------------------------
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Georg Heppner <heppner@fzi.de>
 * \date    2014-5-22
 *
 */
//----------------------------------------------------------------------

#include "schunk_svh_library/websocket/LWA4PState.h"

#include <string>
#include <json/json.h>
#include <json/writer.h>

namespace schunk_svh_library {
namespace websocket {

LWA4PState::LWA4PState(): RobotState(),
  m_movement_state(eST_DEACTIVATED)
{
}


std::string LWA4PState::getStateJSON()
{
  Json::Value message;
  Json::FastWriter json_writer;

  message["type"] = "arm";
  switch (m_movement_state)
  {
  case eST_DEACTIVATED:
      message["state"] = "deactivated";
    break;
  case eST_CONTROLLED:
      message["state"] = "controlled";
    break;
  case eST_FAULT: message["state"] = "fault";
    break;
  case eST_CHAIN_BUILT_UP:
      message["state"] = "chain built up";
    break;
  case eST_IPM_MODE:
      message["state"] = "ipm mode";
    break;
  case eST_ERROR_ACKED: message["state"] = "error cleared";
    break;
  case eST_COMMUTATION_FOUND:
      message["state"] = "commutation found";
    break;
  case eST_PSEUDE_ABSOLUT_CALIBRATED:
      message["state"] = "pseudo absolut calibrated";
    break;
  case eST_JITTER_SYNCED:
      message["state"] = "jitter synced";
    break;
  case eST_SWITCH_OF_POSE_DIFFERS:
      message["state"] = "switch of pose differs";
    break;
  case eST_SWITCH_OF_POSE_VERIFIED:
      message["state"] = "switch of pose verified";
    break;
  case eST_READY:
      message["state"] = "ready";
    break;
  case eST_ENABLED:
      message["state"] = "enabled";
    break;
  case eST_ABSOLUTE_POSITION_LOST:
    message["state"] = "ERROR: Absolute calibration was lost";
    break;
  default:
    message["state"] = "unknown";
  };

  for (jnt_value_map::iterator it=m_joints.begin(); it != m_joints.end(); it++)
  {
    Json::Value joint;
    //joint["id"] = (unsigned int)i++;
    joint["id"] = (unsigned int)it->first;
    joint["position"] = it->second.position;
    joint["speed"] = it->second.velocity;
    joint["leds"] = it->second.error;
    joint["enabled"] = it->second.enabled;
    joint["homed"] = it->second.homed;
    joint["version"] = 0.61; // TODO: don't use fake data
    joint["temp"] = 42.0 + ((2^(unsigned int)it->first)%200)/100.0; // TODO: don't use fake data

    message["data"].append(joint);
  }


  return json_writer.write(message);
}

void LWA4PState::setMovementState(const int &movement_state)
{
  m_movement_state = static_cast<MovementState>(movement_state);
}


}} // end of NS
