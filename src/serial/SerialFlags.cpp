// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Lars Pfotzer <pfotzer@fzi.de>
 * \date    2013-03-08
 *
 * \author  Klaus Uhl
 * \date    2007-11-06
 *
 */
//----------------------------------------------------------------------

#include "schunk_svh_library/serial/SerialFlags.h"

// Terminal headers are included after all Debug headers, because the
// Debug headers may include Eigen/Core from the Eigen matrix library.
// For Eigen3 there is a name clash with B0 from termio.h, and Eigen3
// silently #undefs it.  Therefore we make sure here that termio.h
// gets re-included last.
#ifdef _SYSTEM_POSIX_
#  include <termios.h>
#  ifdef _SYSTEM_DARWIN_
#    define B500000 500000
#    define B921600 921600
#  endif
#endif

namespace driver_svh {
namespace serial {

//----------------------------------------------------------------------
// SerialFlags
//----------------------------------------------------------------------

#ifdef _SYSTEM_POSIX_

unsigned long SerialFlags::cFlags() const
{
  unsigned long cflags = 0;

  switch (m_data_bits)
  {
    case E_DB_5: {
      cflags |= CS5;
      break;
    }
    case E_DB_6: {
      cflags |= CS6;
      break;
    }
    case E_DB_7: {
      cflags |= CS7;
      break;
    }
    case E_DB_8: {
      cflags |= CS8;
      break;
    }
  }

  if (m_stop_bits == E_SB_2)
  {
    cflags |= CSTOPB;
  }

  if (m_parity != E_P_NONE)
  {
    cflags |= PARENB;
  }

  if (m_parity == E_P_ODD)
  {
    cflags |= PARODD;
  }

  cflags |= cFlags(m_baud_rate);


  if (m_flow_control == E_FC_FLOW)
  {
    cflags |= CRTSCTS;
  }

  if (!m_use_modem_control)
  {
    cflags |= CLOCAL;
  }

  if (m_enable_receiver)
  {
    cflags |= CREAD;
  }

  if (m_enable_stop_on_receive)
  {
    cflags |= IXOFF;
  }

  return cflags;
}

unsigned long SerialFlags::cFlags(BaudRate baud_rate)
{
  switch (baud_rate)
  {
    case SerialFlags::E_BR_50:
      return B50;
    case SerialFlags::E_BR_75:
      return B75;
    case SerialFlags::E_BR_110:
      return B110;
    case SerialFlags::E_BR_134:
      return B134;
    case SerialFlags::E_BR_150:
      return B150;
    case SerialFlags::E_BR_200:
      return B200;
    case SerialFlags::E_BR_300:
      return B300;
    case SerialFlags::E_BR_600:
      return B600;
    case SerialFlags::E_BR_1200:
      return B1200;
    case SerialFlags::E_BR_1800:
      return B1800;
    case SerialFlags::E_BR_2400:
      return B2400;
    case SerialFlags::E_BR_4800:
      return B4800;
    case SerialFlags::E_BR_9600:
      return B9600;
    case SerialFlags::E_BR_19200:
      return B19200;
    case SerialFlags::E_BR_38400:
      return B38400;
    case SerialFlags::E_BR_57600:
      return B57600;
    case SerialFlags::E_BR_115200:
      return B115200;
    case SerialFlags::E_BR_230400:
      return B230400;
    case SerialFlags::E_BR_500000:
      return B500000;
    case SerialFlags::E_BR_921600:
      return B921600;
    default:
      return B0;
  }
}

#endif

#ifdef _SYSTEM_WIN32_

BYTE parity_map[] = {NOPARITY, EVENPARITY, ODDPARITY, MARKPARITY, SPACEPARITY};

BYTE stopbit_map[] = {ONESTOPBIT, ONE5STOPBITS, TWOSTOPBITS};

void SerialFlags::GetDCB(LPDCB dcb) const
{
  dcb->DCBlength         = sizeof(DCB);
  dcb->fBinary           = TRUE;
  dcb->fOutxCtsFlow      = FALSE;
  dcb->fOutxDsrFlow      = FALSE;
  dcb->fDtrControl       = DTR_CONTROL_DISABLE;
  dcb->fDsrSensitivity   = FALSE;
  dcb->fTXContinueOnXoff = FALSE;
  dcb->fOutX             = FALSE;
  dcb->fInX              = FALSE;
  dcb->fErrorChar        = FALSE;
  dcb->fNull             = FALSE;
  dcb->fRtsControl       = RTS_CONTROL_DISABLE;
  dcb->fAbortOnError     = FALSE;
  dcb->wReserved         = 0U;

  dcb->ByteSize = m_data_bits;
  dcb->StopBits = stopbit_map[m_stop_bits];
  dcb->fParity  = (m_parity != eP_NONE);
  dcb->Parity   = parity_map[m_parity];
  dcb->BaudRate = m_baud_rate;

  // if (m_flow_control == eFC_FLOW)
  //{
  //  cflags |= CRTSCTS;
  //}
  //
  // if (!m_use_modem_control)
  //{
  //  cflags |= CLOCAL;
  //}
  //
  // if (m_enable_receiver)
  //{
  //  cflags |= CREAD;
  //}
  //
  // if (m_enable_stop_on_receive)
  //{
  //  dcb->fOutX = TRUE;
  //}
}

#endif

} // namespace serial
} // namespace driver_svh
