// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Lars pfotzer
 * \date    2014-01-30
 *
 */
//----------------------------------------------------------------------
#ifndef DRIVER_S5FH_S5FH_FINGER_MANAGER_H_INCLUDED
#define DRIVER_S5FH_S5FH_FINGER_MANAGER_H_INCLUDED

#include "driver_s5fh/ImportExport.h"

namespace driver_s5fh {

/*! This class manages controller parameters and the finger reset.
 */
class DRIVER_S5FH_IMPORT_EXPORT S5FHFingerManager
{
public:
  /*! Constructs a finger manager for the SCHUNK five finger hand.
   */
  S5FHFingerManager();

  ~S5FHFingerManager();

};

}

#endif