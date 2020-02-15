#include "rn2xx3_handler.h"

#include "rn2xx3_helper.h"
#include "rn2xx3_received_types.h"


rn2xx3_handler::rn2xx3_handler(Stream& serial) : _serial(serial)
{
  clearSerialBuffer();
}

String rn2xx3_handler::sendRawCommand(const String& command)
{
  unsigned long timer = millis();

  if (!prepare_raw_command(command)) {
    setLastError(F("sendRawCommand: Prepare fail"));
    return "";
  }

  if (wait_command_finished() == rn2xx3_handler::RN_state::timeout) {
    String log = F("sendRawCommand timeout: ");
    log += command;
    setLastError(log);
  }
  String ret = get_received_data();

  if (_extensive_debug) {
    String log = command;
    log += '(';
    log += String(millis() - timer);
    log += ')';
    setLastError(log);
  }

  ret.trim();
  return ret;
}

bool rn2xx3_handler::prepare_raw_command(const String& command)
{
  if (!command_finished()) {
    // Handling of another command has not finished.
    return false;
  }
  _sendData       = command;
  _processing_cmd = Active_cmd::other;
  _busy_count     = 0;
  _retry_count    = 0;
  set_state(RN_state::command_set_to_send);

  // Set state may set command_finished to true if no _sendData is set.
  return !command_finished();
}

bool rn2xx3_handler::prepare_tx_command(const String& command, const String& data, bool shouldEncode, uint8_t port) {
  int estimatedSize = command.length() + 4; // port + space

  estimatedSize += shouldEncode ? 2 * data.length() : data.length();
  String tmpCommand;
  tmpCommand.reserve(estimatedSize);
  tmpCommand = command;

  if (command.endsWith(F("cnf "))) {
    // No port was given in the command, so add the port.
    tmpCommand += String(port);
    tmpCommand += ' ';
  }

  if (shouldEncode)
  {
    tmpCommand += rn2xx3_helper::base16encode(data);
  }
  else
  {
    tmpCommand += data;
  }

  if (!prepare_raw_command(tmpCommand)) {
    return false;
  }
  _processing_cmd = Active_cmd::TX;
  return true;
}

bool rn2xx3_handler::exec_join(bool useOTAA) {
  if (!command_finished()) {
    // What to return here, whether a join is executed, or if the join was successful?
    return false;
  }
  updateStatus();

  if (prepare_raw_command(useOTAA ? F("mac join otaa") : F("mac join abp"))) {
    _processing_cmd = Active_cmd::join;
    Status.Joined   = false;

    if (wait_command_finished() ==  rn2xx3_handler::RN_state::join_accepted)
    {
      Status.Joined = true;
      saveUpdatedStatus();
    }
  }

  return Status.Joined;
}

rn2xx3_handler::RN_state rn2xx3_handler::async_loop()
{
  if (_state != RN_state::must_pause) {
    if (!command_finished() && time_out_reached()) {
      set_state(RN_state::timeout);
    }
  }


  switch (get_state()) {
    case RN_state::idle:

      // Noting to do.
      break;
    case RN_state::command_set_to_send:
    {
      ++_retry_count;

      // retransmit/retry a maximum of 10 times
      // N.B. this also applies when no_free_ch was received.
      if (_retry_count > 10) {
        set_state(RN_state::max_attempt_reached);
      } else {
        _receivedData = "";
        clearSerialBuffer();

        // Write the commmand
        _serial.print(get_send_data());
        _serial.println();

        set_state(RN_state::wait_for_reply);
      }
      break;
    }
    case RN_state::must_pause:
    {
      // Do not call writes for a while.
      if (time_out_reached()) {
        set_state(RN_state::command_set_to_send);
      }
      break;
    }
    case RN_state::wait_for_reply:
    case RN_state::wait_for_reply_rx2:
    {
      if (read_line()) {
        switch (_state) {
          case RN_state::wait_for_reply:
            set_state(RN_state::reply_received);
            break;
          case RN_state::wait_for_reply_rx2:
            set_state(RN_state::reply_received_rx2);
            break;
          default:

            // Only process data when in the wait for reply state
            break;
        }
      }

      if (_invalid_char_read) {
        set_state(RN_state::invalid_char_read);
      }
      break;
    }
    case RN_state::reply_received:
    case RN_state::reply_received_rx2:
    {
      handle_reply_received();
      break;
    }
    case RN_state::timeout:
    case RN_state::max_attempt_reached:
    case RN_state::error:
    case RN_state::must_perform_init:
    case RN_state::duty_cycle_exceeded:
    case RN_state::invalid_char_read:

      break;

    case RN_state::tx_success:
    case RN_state::tx_success_with_rx:
    case RN_state::reply_received_finished:
    case RN_state::join_accepted:
      break;

      // Do not use default: here, so the compiler warns when a new state is not yet implemented here.
      //    default:
      //      break;
  }
  return get_state();
}

rn2xx3_handler::RN_state rn2xx3_handler::wait_command_finished(unsigned long timeout)
{
  // Still use a timeout to prevent endless loops, although the state machine should always obey the set timeouts.
  unsigned long start_timer = millis();

  while ((millis() - start_timer) < timeout) {
    async_loop();

    if (command_finished()) { return get_state(); }
    delay(10);
  }
  return get_state();
}

rn2xx3_handler::RN_state rn2xx3_handler::wait_command_accepted(unsigned long timeout)
{
  // Still use a timeout to prevent endless loops, although the state machine should always obey the set timeouts.
  unsigned long start_timer = millis();

  while ((millis() - start_timer) < timeout) {
    async_loop();

    if (command_finished() || (get_state() == RN_state::wait_for_reply_rx2)) {
      return get_state();
    }
    delay(10);
  }
  return get_state();
}

bool rn2xx3_handler::command_finished() const
{
  return _processing_cmd == Active_cmd::none;
}

const String& rn2xx3_handler::get_send_data() const {
  return _sendData;
}

const String& rn2xx3_handler::get_received_data() const {
  return _receivedData;
}

const String& rn2xx3_handler::get_received_data(unsigned long& duration) const {
  duration = millis() - _start_prep;
  return _receivedData;
}

const String& rn2xx3_handler::get_rx_message() const {
  return _rxMessenge;
}

String rn2xx3_handler::peekLastError() const
{
  return _lastError;
}

String rn2xx3_handler::getLastError()
{
  String res = _lastError;

  _lastError = "";
  return res;
}

void rn2xx3_handler::setLastError(const String& error)
{
  if (_extensive_debug) {
    _lastError += '\n';
    _lastError += String(millis());
    _lastError += F(" : ");
    _lastError += error;
  } else {
    _lastError = error;
  }
}

rn2xx3_handler::RN_state rn2xx3_handler::get_state() const {
  return _state;
}

bool rn2xx3_handler::getRxDelayValues(uint32_t& rxdelay1,
                                      uint32_t& rxdelay2)
{
  rxdelay1 = _rxdelay1;
  rxdelay2 = _rxdelay2;
  return _rxdelay1 != 0 && _rxdelay2 != 0;
}

void rn2xx3_handler::set_state(rn2xx3_handler::RN_state state) {
  const bool was_processing_cmd = _processing_cmd != Active_cmd::none;

  _state = state;

  switch (state) {
    case RN_state::wait_for_reply:
    case RN_state::wait_for_reply_rx2:
    {
      // We will wait for data, so make sure the receiving buffer is empty.
      _receivedData = "";

      if (state == RN_state::wait_for_reply_rx2)
      {
        // Enough time to wait for:
        // Transmit Time On Air + receive_delay2 + receiving RX2 packet.
        switch (_processing_cmd) {
          case Active_cmd::join:
            set_timeout(10000);            // Do take a bit more time for a join.
            break;
          case Active_cmd::TX:
            set_timeout(_rxdelay2 + 3000); // 55 bytes @EU868 data rate of SF12/125kHz = 2,957.31 milliseconds
            break;
          default:

            // Other commands do not use RX2
            break;
        }
      }
      break;
    }
    case RN_state::reply_received:
    case RN_state::reply_received_rx2:

      // Nothing to set here, as we will now inspect the received data and not communicate with the module.
      break;
    case RN_state::command_set_to_send:

      if (_sendData.length() == 0) {
        set_state(RN_state::idle);
      } else {
        _start_prep = millis();

        set_timeout(1500); // Roughly 1100 msec needed for mac save
                           // Almost all other commands reply in 20 - 100 msec.
      }

      break;
    case RN_state::must_pause:
      set_timeout(1000);
      break;

    case RN_state::invalid_char_read:

      if (_processing_cmd == Active_cmd::other) {
        // Must retry to run the command again.
        set_state(RN_state::command_set_to_send);
      } else {
        _processing_cmd = Active_cmd::none;
      }
      break;

    case RN_state::idle:

      // ToDo: Add support for sleep mode.
      // Clear the strings to free up some memory.
      _processing_cmd = Active_cmd::none;
      _sendData       = "";
      _receivedData   = "";
      _rxMessenge     = "";
      _lastError      = "";
      break;
    case RN_state::timeout:
    case RN_state::max_attempt_reached:
    case RN_state::error:
    case RN_state::must_perform_init:
    case RN_state::duty_cycle_exceeded:

      // We cannot continue from this error
      _processing_cmd = Active_cmd::none;
      break;
    case RN_state::tx_success:
    case RN_state::tx_success_with_rx:
    case RN_state::reply_received_finished:
    case RN_state::join_accepted:
      _processing_cmd = Active_cmd::none;
      break;

      // Do not use default: here, so the compiler warns when a new state is not yet implemented here.
      //    default:
      //      break;
  }

  if (was_processing_cmd && (_processing_cmd == Active_cmd::none)) {
    _start             = 0;
    _invalid_char_read = false;
    _busy_count        = 0;
    _retry_count       = 0;
  }
}

bool rn2xx3_handler::read_line()
{
  while (_serial.available()) {
    int c = _serial.read();

    if (c >= 0) {
      const char character = static_cast<char>(c & 0xFF);

      if (!rn2xx3_helper::valid_char(character)) {
        _invalid_char_read = true;
        return false;
      }

      _receivedData += character;

      if (character == '\n') {
        return true;
      }
    }
  }
  return false;
}

void rn2xx3_handler::set_timeout(unsigned long timeout)
{
  _timeout = timeout;
  _start   = millis();
}

bool rn2xx3_handler::time_out_reached() const
{
  return (millis() - _start) >= _timeout;
}

void rn2xx3_handler::clearSerialBuffer()
{
  while (_serial.available()) {
    _serial.read();
  }
}

bool rn2xx3_handler::updateStatus()
{
  const String status_str = sendRawCommand(F("mac get status"));

  if (!rn2xx3_helper::isHexStr_of_length(status_str, 8)) {
    String error = F("mac get status  : No valid hex string \"");
    error += status_str;
    error += '\"';
    setLastError(error);
    return false;
  }
  uint32_t status_value = strtoul(status_str.c_str(), 0, 16);
  Status.decode(status_value);

  if ((_rxdelay1 == 0) || (_rxdelay2 == 0) || Status.SecondReceiveWindowParamUpdated)
  {
    readUIntMacGet(F("rxdelay1"), _rxdelay1);
    readUIntMacGet(F("rxdelay2"), _rxdelay2);
    Status.SecondReceiveWindowParamUpdated = false;
  }
  return true;
}

bool rn2xx3_handler::saveUpdatedStatus()
{
  // Only save to the eeprom when really needed.
  // No need to store the current config when there is no active connection.
  // Todo: Must keep track of last saved counters and decide to update when current counter differs more than set threshold.
  bool saved = false;

  if (updateStatus())
  {
    if (Status.Joined && !Status.RejoinNeeded && Status.saveSettingsNeeded())
    {
      saved = RN2xx3_received_types::determineReceivedDataType(sendRawCommand(F("mac save"))) == RN2xx3_received_types::ok;
      Status.clearSaveSettingsNeeded();
      updateStatus();
    }
  }
  return saved;
}

void rn2xx3_handler::handle_reply_received() {
  const RN2xx3_received_types::received_t received_datatype = RN2xx3_received_types::determineReceivedDataType(_receivedData);

  // Check if the reply is unexpected, so log the command + reply
  bool mustLogAsError = _extensive_debug;

  switch (received_datatype) {
    case RN2xx3_received_types::ok:
    case RN2xx3_received_types::UNKNOWN: // Many get-commands just return a value, so that will be of type UNKNOWN
    case RN2xx3_received_types::accepted:
    case RN2xx3_received_types::mac_tx_ok:
    case RN2xx3_received_types::mac_rx:
    case RN2xx3_received_types::radio_rx:
    case RN2xx3_received_types::radio_tx_ok:
      break;

    default:
      mustLogAsError = true;
      break;
  }

  if (mustLogAsError) {
    String error;
    error.reserve(_sendData.length() + _receivedData.length() + 4);

    if (_processing_cmd == Active_cmd::TX) {
      // TX commands are a lot longer, so do not include complete command
      error += F("mac tx");
    } else {
      error += _sendData;
    }
    error += F(" -> ");
    error += _receivedData;
    setLastError(error);
  }

  switch (received_datatype) {
    case RN2xx3_received_types::UNKNOWN:

      // A reply which is not part of standard replies, so it can be a requested value.
      // Command is now finished.
      set_state(RN_state::reply_received_finished);
      break;
    case RN2xx3_received_types::ok:
    {
      const bool expect_rx2 =
        (_processing_cmd == Active_cmd::TX) ||
        (_processing_cmd == Active_cmd::join);

      if ((get_state() == RN_state::reply_received) && expect_rx2) {
        // "mac tx" and "join otaa" commands may receive a second response if the first one was "ok"
        set_state(RN_state::wait_for_reply_rx2);
      } else {
        set_state(RN_state::reply_received_finished);
      }
      break;
    }

    case RN2xx3_received_types::invalid_param:
    {
      // parameters (<type> <portno> <data>) are not valid
      // should not happen if we typed the commands correctly
      set_state(RN_state::error);
      break;
    }

    case RN2xx3_received_types::not_joined:
    {
      // the network is not joined
      Status.Joined = false;
      set_state(RN_state::must_perform_init);
      break;
    }

    case RN2xx3_received_types::no_free_ch:
    {
      // all channels are busy
      // probably duty cycle limits exceeded.
      // User must retry.
      set_state(RN_state::duty_cycle_exceeded);
      break;
    }

    case RN2xx3_received_types::silent:
    {
      // the module is in a Silent Immediately state
      // This is enforced by the network.
      // To enable:
      // sendRawCommand(F("mac forceENABLE"));
      // N.B. One has to think about why this has happened.
      set_state(RN_state::must_perform_init);
      break;
    }

    case RN2xx3_received_types::frame_counter_err_rejoin_needed:
    {
      // the frame counter rolled over
      set_state(RN_state::must_perform_init);
      break;
    }

    case RN2xx3_received_types::busy:
    {
      // MAC state is not in an Idle state
      _busy_count++;

      // Not sure if this is wise. At low data rates with large packets
      // this can perhaps cause transmissions at more than 1% duty cycle.
      // Need to calculate the correct constant value.
      // But it is wise to have this check and re-init in case the
      // lorawan stack in the RN2xx3 hangs.
      if (_busy_count >= 10)
      {
        set_state(RN_state::must_perform_init);
      }
      else
      {
        delay(1000);
      }
      break;
    }

    case RN2xx3_received_types::mac_paused:
    {
      // MAC was paused and not resumed back
      set_state(RN_state::must_perform_init);
      break;
    }

    case RN2xx3_received_types::invalid_data_len:
    {
      if (_state == RN_state::reply_received)
      {
        // application payload length is greater than the maximum application payload length corresponding to the current data rate
      }
      else
      {
        // application payload length is greater than the maximum application payload length corresponding to the current data rate.
        // This can occur after an earlier uplink attempt if retransmission back-off has reduced the data rate.
      }
      set_state(RN_state::error);
      break;
    }

    case RN2xx3_received_types::mac_tx_ok:
    {
      // if uplink transmission was successful and no downlink data was received back from the server
      // SUCCESS!!
      set_state(RN_state::tx_success);
      break;
    }

    case RN2xx3_received_types::mac_rx:
    {
      // mac_rx <portno> <data>
      // transmission was successful
      // <portno>: port number, from 1 to 223
      // <data>: hexadecimal value that was received from theserver
      // example: mac_rx 1 54657374696E6720313233
      _rxMessenge = _receivedData.substring(_receivedData.indexOf(' ', 7) + 1);
      set_state(RN_state::tx_success_with_rx);
      break;
    }

    case RN2xx3_received_types::mac_err:
    {
      set_state(RN_state::must_perform_init);
      break;
    }

    case RN2xx3_received_types::radio_err:
    {
      // transmission was unsuccessful, ACK not received back from the server
      // This should never happen. If it does, something major is wrong.
      set_state(RN_state::must_perform_init);
      break;
    }

    case RN2xx3_received_types::accepted:
      set_state(RN_state::join_accepted);
      break;


    case RN2xx3_received_types::denied:
    case RN2xx3_received_types::keys_not_init:
      set_state(RN_state::error);
      break;

    case RN2xx3_received_types::radio_rx:
    case RN2xx3_received_types::radio_tx_ok:

      // FIXME TD-er: Not sure what to do here.
      break;


      /*
         default:
         {
         // unknown response after mac tx command
         set_state(RN_state::must_perform_init);
         break;
         }
       */
  }
}

bool rn2xx3_handler::readUIntMacGet(const String& param, uint32_t& value)
{
  String command;

  command.reserve(8 + param.length());
  command  = F("mac get ");
  command += param;
  String value_str = sendRawCommand(command);

  if (value_str.length() == 0)
  {
    return false;
  }
  value = strtoul(value_str.c_str(), 0, 10);
  return true;
}

bool rn2xx3_handler::sendMacSet(const String& param, const String& value)
{
  String command;

  command.reserve(10 + param.length() + value.length());
  command  = F("mac set ");
  command += param;
  command += ' ';
  command += value;

  if (_extensive_debug) {
    setLastError(command);
  }

  return RN2xx3_received_types::determineReceivedDataType(sendRawCommand(command)) == RN2xx3_received_types::ok;
}

bool rn2xx3_handler::sendMacSetEnabled(const String& param, bool enabled)
{
  return sendMacSet(param, enabled ? F("on") : F("off"));
}

bool rn2xx3_handler::sendMacSetCh(const String& param, unsigned int channel, const String& value)
{
  String command;

  command.reserve(20);
  command  = param;
  command += ' ';
  command += channel;
  command += ' ';
  command += value;
  return sendMacSet(F("ch"), command);
}

bool rn2xx3_handler::sendMacSetCh(const String& param, unsigned int channel, uint32_t value)
{
  return sendMacSetCh(param, channel, String(value));
}

bool rn2xx3_handler::setChannelDutyCycle(unsigned int channel, unsigned int dutyCycle)
{
  return sendMacSetCh(F("dcycle"), channel, dutyCycle);
}

bool rn2xx3_handler::setChannelFrequency(unsigned int channel, uint32_t frequency)
{
  return sendMacSetCh(F("freq"), channel, frequency);
}

bool rn2xx3_handler::setChannelDataRateRange(unsigned int channel, unsigned int minRange, unsigned int maxRange)
{
  String value;

  value  = String(minRange);
  value += ' ';
  value += String(maxRange);
  return sendMacSetCh(F("drrange"), channel, value);
}

bool rn2xx3_handler::setChannelEnabled(unsigned int channel, bool enabled)
{
  return sendMacSetCh(F("status"), channel, enabled ? F("on") : F("off"));
}

bool rn2xx3_handler::set2ndRecvWindow(unsigned int dataRate, uint32_t frequency)
{
  String value;

  value  = String(dataRate);
  value += ' ';
  value += String(frequency);
  return sendMacSet(F("rx2"), value);
}

bool rn2xx3_handler::setAdaptiveDataRate(bool enabled)
{
  return sendMacSetEnabled(F("adr"), enabled);
}

bool rn2xx3_handler::setAutomaticReply(bool enabled)
{
  return sendMacSetEnabled(F("ar"), enabled);
}

bool rn2xx3_handler::setTXoutputPower(int pwridx)
{
  // Possible values:

  /*
     433 MHz EU:
     0: 10 dBm
     1:  7 dBm
     2:  4 dBm
     3:  1 dBm
     4: -2 dBm
     5: -5 dBm

     868 MHz EU:
     0: N/A
     1: 14 dBm
     2: 11 dBm
     3: 8 dBm
     4: 5 dBm
     5: 2 dBm

     900 MHz US/AU:
     5 : 20 dBm
     7 : 16 dBm
     8 : 14 dBm
     9 : 12 dBm
     10: 10 dBm
   */
  return sendMacSet(F("pwridx"), String(pwridx));
}
