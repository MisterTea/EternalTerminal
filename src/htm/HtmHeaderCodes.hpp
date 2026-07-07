#ifndef __HTM_HEADER_CODES_H__
#define __HTM_HEADER_CODES_H__

/**
 * @brief Header bytes that annotate every HTM protocol message.
 *
 * The client and server use these single-byte identifiers to route payloads.
 */
/** @brief Client -> server: inject characters into a pane. */
const char INSERT_KEYS = '1';
/** @brief Server -> client: sends the serialized multiplexer state. */
const char INIT_STATE = '2';
/** @brief Client -> server: requests closing a pane. */
const char CLIENT_CLOSE_PANE = '3';
/** @brief Server -> client: streams pane output. */
const char APPEND_TO_PANE = '4';
/** @brief Client -> server: opens a new tab/pane. */
const char NEW_TAB = '5';
/** @brief Server -> client: tells the UI that a pane was removed. */
const char SERVER_CLOSE_PANE = '8';
/** @brief Client -> server: creates a split pane layout. */
const char NEW_SPLIT = '9';
/** @brief Client -> server: resizes a pane. */
const char RESIZE_PANE = 'A';
/** @brief Server -> client: send debug log lines to the terminal. */
const char DEBUG_LOG = 'B';
/** @brief Server -> client: listen for developer commands (shutdown, escape).
 */
const char INSERT_DEBUG_KEYS = 'C';
/** @brief Closes the HTM session (handshake end-of-stream). */
const char SESSION_END = 'D';

#endif
