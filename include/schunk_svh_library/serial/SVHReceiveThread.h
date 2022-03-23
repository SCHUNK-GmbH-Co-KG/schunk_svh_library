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
 * \date    2014-01-30
 * \date    2014-07-16
 *
 * This file contains the ReceiveThread for the serial communication.  In order
 * to receive packages independently from the sending direction, instantiate
 * this class in client code and call its run() method in a separate thread.
 *
 * This class will then poll the serial interface periodically for new data. If
 * data is present, a statemachine will evaluate the right packet structure and
 * send the data via callback to the caller for further parsing once a complete
 * serial packaged is received.
 */
//----------------------------------------------------------------------
#ifndef DRIVER_SVH_SVH_RECEIVE_THREAD_H_INCLUDED
#define DRIVER_SVH_SVH_RECEIVE_THREAD_H_INCLUDED

#include <schunk_svh_library/serial/ByteOrderConversion.h>
#include <schunk_svh_library/serial/Serial.h>

#include <schunk_svh_library/serial/SVHSerialPacket.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

using driver_svh::serial::Serial;

namespace driver_svh {

//! definition of function callback for received packages
using ReceivedPacketCallback =
  std::function<void(const SVHSerialPacket& packet, unsigned int packet_count)>;

/*!
 * \brief Class for receiving messages from the serial device.
 *
 * Instantiate this class in client code and call its run() method in a separate thread.
 * Data is passed to the caller via the provided callback.
 */
class SVHReceiveThread
{
public:
  /*!
   * \brief SVHReceiveThread Constructs a new Receivethread
   * \param idle_sleep sleep time during run() if no data is available
   * \param device handle of the serial device
   * \param received_callback function to call uppon finished packet
   */
  SVHReceiveThread(const std::chrono::microseconds& idle_sleep,
                   std::shared_ptr<Serial> device,
                   ReceivedPacketCallback const& received_callback);

  //! Default DTOR
  ~SVHReceiveThread() {}

  //! run method of the thread, executes the main program in an infinite loop
  void run();

  //! stop the run() method
  void stop() { m_continue = false; };

  //! return the count of received packets
  unsigned int receivedPacketCount() { return m_packets_received; }

  /*!
   * \brief resetReceivedPackageCount Resets the received package count to zero. This can be usefull
   * to set all communication variables to the initial state
   */
  void resetReceivedPackageCount() { m_packets_received = 0; }

private:
  //! Flag to end the run() method from external callers
  std::atomic<bool> m_continue{true};

  //! sleep time during run() if idle
  std::chrono::microseconds m_idle_sleep;

  //! pointer to serial device object
  std::shared_ptr<Serial> m_serial_device;

  //! enum for receive packet state machine states
  enum
  {
    E_RS_HEADE_R1,
    E_RS_HEADE_R2,
    E_RS_INDEX,
    E_RS_ADDRESS,
    E_RS_LENGT_H1,
    E_RS_LENGT_H2,
    E_RS_DATA,
    E_RS_CHECKSU_M1,
    E_RS_CHECKSU_M2
  } typedef tState;

  //! current state of the state machine
  tState m_received_state;

  //! length of received serial data
  uint16_t m_length;

  //! Checksum of packet
  uint8_t m_checksum1;
  uint8_t m_checksum2;

  //! length of received serial data
  std::vector<uint8_t> m_data;

  //! pointer to array builder object for packet receive
  driver_svh::ArrayBuilder m_ab;

  //! packets counter
  std::atomic<unsigned int> m_packets_received;

  //! counter for skipped bytes in case no packet is detected
  unsigned int m_skipped_bytes;

  //! state machine processing received data
  bool receiveData();

  //! function callback for received packages
  ReceivedPacketCallback m_received_callback;
};

} // namespace driver_svh

#endif
