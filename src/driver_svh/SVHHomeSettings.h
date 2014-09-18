// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-


// -- BEGIN LICENSE BLOCK ----------------------------------------------
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Georg Heppner <heppner@fzi.de>
 * \date    2014-9-18
 *
 * This file contains the HomeSettings data structure that is used to
 * specify the min and max positions as well as the homing offset used during reset and the
 * movement direction of the fingers
 */
//----------------------------------------------------------------------

#ifndef SVHHOMESETTINGS_H
#define SVHHOMESETTINGS_H

namespace driver_svh {

//! data sctructure for home positions
struct SVHHomeSettings
{
  //! Movement direction of the finger +1 or -1 home in positive or negative direction
  int   direction;
  //! Minimum reachable tick limit, given as offset from the hard stop (soft limit)
  float minimumOffset;
  //! Maximum reachable tick limt, given as an offset from the hard stop (soft limit)
  float maximumOffset;
  //! Idle position to move the fingers to after initialization @note This position should be within the min and max offset positions.
  float idlePosition;
  //! The total angle in [rad] that is used by the finger i.e. the anlge between one (soft) stop and the other This value will be used for calculation of the ticks
  //! @note Currently we use the HARD STOP to HARD STOP angle but map that to a range of ticks from SOFT STOP to SOFT STOP this might be the wrong approach
  //! @note as we therefore would get an angle of zero event hough the finger might be standing at 5k ticks. However by using this method we can asure the Range of the
  //! @note input to be of constant size. This night change to better reflect the real rad value
  float rangeRad;

  //!
  //! \brief SVHHomeSettings Default constructor initializing empty homeSettings. This is not usefull and should be filled immediately after
  //!
  SVHHomeSettings():
    direction(+1),
    minimumOffset(0.0),
    maximumOffset(0.0),
    idlePosition(0.0),
    rangeRad(0.0)
  {}

  //!
  //! \brief SVHHomeSettingsn Constructor for the home settings, providing each value individually
  //! \param direction_ Reset direction +1 oder -1
  //! \param minimumOffset_ soft stop minimum value
  //! \param maximumOffset_ soft stop maximum value
  //! \param idlePosition_ position to go to after reset
  //! \param rangeRad_ range of moevement in rad
  //!
  SVHHomeSettings(const int &direction_,const float &minimumOffset_,const float &maximumOffset_,const float &idlePosition_,const float &rangeRad_):
    minimumOffset(minimumOffset_),
    maximumOffset(maximumOffset_),
    idlePosition(idlePosition_),
    rangeRad(rangeRad_)
  {
    // Limit the direction to just contain a factor rather than any multipliers
    direction = direction_ < 0 ? -1 : +1;
  }

  //!
  //! \brief SVHHomeSettings Convenience constructor to easily construct home settings out of a singe vector. Values are evaluated in order and if the vector is to short, values will be filles with zeros
  //! \param home_settings vector of floats containing the values for the home settings. If the vector is to short the remaining values will be filled with zeros. If it is to long only as much elements as there are values in HomeSettings will be evaluated
  //!
  SVHHomeSettings(const std::vector<float> & home_settings)
  {
    // maybe not the most beautifull way but it works.
    size_t size = home_settings.size();
    float temp_direction = 0.0;

    temp_direction = (size > 0) ? home_settings[0] : 0.0;
    direction = temp_direction < 0.0 ? -1 : +1;

    minimumOffset = (size > 1) ? home_settings[1] : 0.0;
    maximumOffset = (size > 2) ? home_settings[2] : 0.0;
    idlePosition  = (size > 3) ? home_settings[3] : 0.0;
    rangeRad  = (size > 4)     ? home_settings[4] : 0.0;
  }

};


//! Output stream operator to easily print position settings
inline std::ostream& operator << (std::ostream& o, const SVHHomeSettings& hs)
{
  o << "Direction " << hs.direction << " "
    << "Min offset " << hs.minimumOffset << " "
    << "Max offset "<< hs.maximumOffset << " "
    << "idle pos "  << hs.idlePosition  << " "
    << "Range Rad " << hs.rangeRad << " "
    << std::endl;
  return o;
}


}


#endif // SVHHOMESETTINGS_H
