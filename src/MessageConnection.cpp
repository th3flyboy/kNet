/* Copyright 2010 Jukka Jyl�nki

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

/** @file MessageConnection.cpp
	@brief */

#include <algorithm>
#include <iostream>
#include <cassert>

#include "kNet/MessageConnection.h"

#include "kNet/PolledTimer.h"
#include "kNet/Sort.h"
#include "kNet/BitOps.h"

#include "kNet/Network.h"
#include "kNet/NetworkLogging.h"
#include "kNet/DataSerializer.h"
#include "kNet/DataDeserializer.h"
#include "kNet/VLEPacker.h"
#include "kNet/FragmentedTransferManager.h"
#include "kNet/NetworkServer.h"
#include "kNet/Clock.h"
#include "kNet/NetworkWorkerThread.h"

#include "kNet/DebugMemoryLeakCheck.h"

using namespace std;

namespace
{

	/// The interval at which we send ping messages.
	///\todo Make this user-defineable.
	const float pingIntervalMSecs = 3.5 * 1000.f;
	/// The interval at which we update the internal statistics fields.
	const float statsRefreshIntervalMSecs = 1000.f;
	/// The time interval after which, if we don't get a response to a PingRequest message, the connection is declared lost.
	///\todo Make this user-defineable.
	const float connectionLostTimeout = 15.f * 1000.f;

	const float cConnectTimeOutMSecs = 15 * 1000.f; ///< \todo Actually use this time limit.

	const float cDisconnectTimeOutMSecs = 5 * 1000.f; ///< \todo Actually use this time limit.
}

namespace kNet
{

void AppendU8ToVector(std::vector<char> &data, unsigned long value)
{
	data.insert(data.end(), (const char *)&value, (const char *)&value + 1);
}

void AppendU32ToVector(std::vector<char> &data, unsigned long value)
{
	data.insert(data.end(), (const char *)&value, (const char *)&value + 4);
}

std::string ConnectionStateToString(ConnectionState state)
{
	switch(state)
	{
	case ConnectionPending: return "ConnectionPending";
	case ConnectionOK: return "ConnectionOK";
	case ConnectionDisconnecting: return "ConnectionDisconnecting";
	case ConnectionPeerClosed: return "ConnectionPeerClosed";
	case ConnectionClosed: return "ConnectionClosed";
	default: assert(false); return "(Unknown connection state)";
	}
}

MessageConnection::MessageConnection(Network *owner_, NetworkServer *ownerServer_, Socket *socket_, ConnectionState startingState)
:owner(owner_), ownerServer(ownerServer_), inboundMessageHandler(0), socket(socket_), 
bOutboundSendsPaused(false),
outboundAcceptQueue(256*1024), inboundMessageQueue(512*1024), 
rtt(0.f), packetsInPerSec(0), packetsOutPerSec(0), 
msgsInPerSec(0), msgsOutPerSec(0), bytesInPerSec(0), bytesOutPerSec(0),
lastHeardTime(Clock::Tick()), outboundMessageNumberCounter(0), outboundReliableMessageNumberCounter(0),
outboundQueue(16 * 1024), workerThread(0)
#ifdef THREAD_CHECKING_ENABLED
,workerThreadId(Thread::NullThreadId())
#endif
{
	connectionState = startingState;

	eventMsgsOutAvailable = CreateNewEvent(EventWaitSignal);
	assert(eventMsgsOutAvailable.IsValid());

	Initialize();
}

MessageConnection::~MessageConnection()
{
	LOG(LogObjectAlloc, "Deleting MessageConnection %p.", this);
	if (owner)
		owner->CloseConnection(this);
	FreeMessageData();
	eventMsgsOutAvailable.Close();

	// We can't have a worker thread referencing to this connection any more, since it would
	// be accessing a dangling pointer. Calling owner->CloseConnection above should remove the workerThread.
	assert(workerThread == 0);
}

ConnectionState MessageConnection::GetConnectionState() const
{
	// If we have now low-level socket at all, we have been already deinitialized.
	if (!socket)
		return ConnectionClosed;
	// If the connection is still pending, socket is not read or write open, but we should not think
	// the connection is closed though.
	if (connectionState == ConnectionPending)
		return ConnectionPending;
	if (!socket->IsReadOpen() && !socket->IsWriteOpen())
		return ConnectionClosed;
	if (!socket->IsReadOpen())
		return ConnectionPeerClosed;
	if (!socket->IsWriteOpen())
		return ConnectionDisconnecting;

	return connectionState;
}

bool MessageConnection::IsReadOpen() const
{
	if (NumInboundMessagesPending() > 0)
		return true;
	if (socket && socket->IsOverlappedReceiveReady())
		return true;
	if (GetConnectionState() == ConnectionPeerClosed || GetConnectionState() == ConnectionClosed)
		return false;
	if (socket)
		return socket->IsReadOpen();
	return false;
}

bool MessageConnection::IsWriteOpen() const
{ 
	return socket && socket->IsWriteOpen() && 
		GetConnectionState() != ConnectionDisconnecting && GetConnectionState() != ConnectionClosed;
}

bool MessageConnection::IsPending() const
{
	if (!socket)
		return false;
	return GetConnectionState() == ConnectionPending;
}

void MessageConnection::RunModalClient()
{
	AssertInMainThreadContext();

	while(GetConnectionState() != ConnectionClosed)
	{
		Process();

		///\todo WSACreateEvent/WSAWaitForMultipleEvents for improved responsiveness and performance.
		Clock::Sleep(10);
	}
}

bool MessageConnection::WaitToEstablishConnection(int maxMSecsToWait)
{
	AssertInMainThreadContext();

	if (!IsPending())
		return Connected();
	if (GetConnectionState() != ConnectionPending)
		return false;

	PolledTimer timer((float)maxMSecsToWait);
	while(GetConnectionState() == ConnectionPending && !timer.Test())
		Clock::Sleep(1); ///\todo Instead of waiting multiple 1msec slices, should wait for proper event.

	LOG(LogWaits, "MessageConnection::WaitToEstablishConnection: Waited %f msecs for connection. Result: %s.",
		timer.MSecsElapsed(), ConnectionStateToString(GetConnectionState()).c_str());

	return GetConnectionState() == ConnectionOK;
}

void MessageConnection::Disconnect(int maxMSecsToWait)
{
	AssertInMainThreadContext();

	if (!socket || !socket->IsWriteOpen())
		return;
/*
	// First, check the actual status of the Socket, and update the connectionState of this MessageConnection accordingly.
	///\todo Avoid this inconsistency of having to propagate socket state to MessageConnection state.
	if (!socket->IsReadOpen() && !socket->IsWriteOpen())
		connectionState = ConnectionClosed;
	if (!socket->IsReadOpen() && connectionState != ConnectionClosed)
		connectionState = ConnectionPeerClosed;
	if (!socket->IsWriteOpen() && connectionState != ConnectionClosed)
		connectionState = ConnectionDisconnecting;
*/
	if (connectionState == ConnectionClosed || connectionState == ConnectionDisconnecting)
		return;

	LOG(LogInfo, "MessageConnection::Disconnect(%d msecs): Write-closing connection. connectionState = %s, socket readOpen:%s, socket writeOpen:%s.", 
		maxMSecsToWait, ConnectionStateToString(connectionState).c_str(), socket->IsReadOpen() ? "true":"false",
		socket->IsWriteOpen() ? "true":"false");
	assert(maxMSecsToWait >= 0);

	PerformDisconnection();
/*
	switch(connectionState)
	{
	case ConnectionPending:
		LOG(LogVerbose, "MessageConnection::Disconnect called when in ConnectionPending state! %s", ToString().c_str());
		// Intentional fall-through.
	case ConnectionOK:
		LOG(LogInfo, "MessageConnection::Disconnect. Write-closing connection %s.", ToString().c_str());
		PerformDisconnection();
		connectionState = ConnectionDisconnecting;
		break;
	case ConnectionDisconnecting:
		LOG(LogVerbose, "MessageConnection::Disconnect. Already disconnecting. %s", ToString().c_str());
		break;
	case ConnectionPeerClosed: // The peer has already signalled it will not send any more data.
		PerformDisconnection();
		connectionState = ConnectionClosed;
		break;
	case ConnectionClosed:
		LOG(LogVerbose, "MessageConnection::Disconnect. Already closed connection. %s", ToString().c_str());
		return;
	default:
		LOG(LogError, "ERROR! MessageConnection::Disconnect called when in an unknown state! %s", ToString().c_str());
		connectionState = ConnectionClosed;
		break;
	}
*/
	if (maxMSecsToWait > 0)
	{
		PolledTimer timer((float)maxMSecsToWait);
		while(socket && socket->IsWriteOpen() && !timer.Test())
		{
			Clock::Sleep(1); ///\todo Instead of waiting multiple 1msec slices, should wait for proper event.
/*
			///\todo Avoid this inconsistency of having to propagate socket state to MessageConnection state.
			if (!socket->IsReadOpen() && !socket->IsWriteOpen())
				connectionState = ConnectionClosed;
			if (!socket->IsReadOpen() && connectionState != ConnectionClosed)
				connectionState = ConnectionPeerClosed;
			if (!socket->IsWriteOpen() && connectionState != ConnectionClosed)
				connectionState = ConnectionDisconnecting;
*/
		}

		LOG(LogWaits, "MessageConnection::Disconnect: Waited %f msecs for disconnection. Result: %s.",
			timer.MSecsElapsed(), ConnectionStateToString(GetConnectionState()).c_str());
	}

	if (GetConnectionState() == ConnectionClosed)
		Close(0);
}

void MessageConnection::Close(int maxMSecsToWait) // [main thread]
{
	AssertInMainThreadContext();

//	if (!socket || (!socket->IsReadOpen() && !socket->IsWriteOpen()) || connectionState == ConnectionClosed)
//		return;
/*
	// First, check the actual status of the Socket, and update the connectionState of this MessageConnection accordingly.
	///\todo Avoid this inconsistency of having to propagate socket state to MessageConnection state.
	if (!socket->IsReadOpen() && !socket->IsWriteOpen())
		connectionState = ConnectionClosed;
	if (!socket->IsReadOpen() && connectionState != ConnectionClosed)
		connectionState = ConnectionPeerClosed;
	if (!socket->IsWriteOpen() && connectionState != ConnectionClosed)
		connectionState = ConnectionDisconnecting;
*/
	if (maxMSecsToWait > 0 && socket && socket->IsWriteOpen())
	{
		Disconnect(maxMSecsToWait);
		LOG(LogInfo, "MessageConnection::Close(%d msecs): Disconnecting. connectionState = %s, readOpen:%s, writeOpen:%s.", 
			maxMSecsToWait, ConnectionStateToString(connectionState).c_str(), (socket && socket->IsReadOpen()) ? "true":"false",
			(socket && socket->IsWriteOpen()) ? "true":"false");
	}

	if (owner)
	{
		LOG(LogInfo, "MessageConnection::Close: Closed connection to %s.", ToString().c_str());
		owner->CloseConnection(this);
		owner = 0;
		ownerServer = 0;
	}

	if (socket && socket->IsReadOpen())
	{
		socket->Close();
		socket = 0;
		assert(!IsWorkerThreadRunning());
	}

	connectionState = ConnectionClosed;

	if (outboundAcceptQueue.Size() > 0)
		LOG(LogVerbose, "MessageConnection::Close(): Had %d messages in outboundAcceptQueue!", (int)outboundAcceptQueue.Size());

	if (outboundQueue.Size() > 0)
		LOG(LogVerbose, "MessageConnection::Close(): Had %d messages in outboundQueue!", (int)outboundQueue.Size());

	if (inboundMessageQueue.Size() > 0)
		LOG(LogVerbose, "MessageConnection::Close(): Had %d messages in inboundMessageQueue!", (int)inboundMessageQueue.Size());

	if (fragmentedSends.UnsafeGetValue().transfers.size() > 0)
		LOG(LogVerbose, "MessageConnection::Close(): Had %d messages in fragmentedSends.transfers list!", (int)fragmentedSends.UnsafeGetValue().transfers.size());

	if (fragmentedReceives.transfers.size() > 0)
		LOG(LogVerbose, "MessageConnection::Close(): Had %d messages in fragmentedReceives.transfers list!", (int)fragmentedReceives.transfers.size());

	FreeMessageData();
}

void MessageConnection::PauseOutboundSends()
{
	AssertInMainThreadContext();

	eventMsgsOutAvailable.Reset();
	bOutboundSendsPaused = true;
}

void MessageConnection::ResumeOutboundSends()
{
	AssertInMainThreadContext();

	bOutboundSendsPaused = false;
	if (NumOutboundMessagesPending() > 0)
		eventMsgsOutAvailable.Set();
}

void MessageConnection::SetPeerClosed()
{
	AssertInWorkerThreadContext();

	switch(connectionState)
	{
	case ConnectionPending:
		LOG(LogVerbose, "Peer closed connection when in ConnectionPending state!"); 
		connectionState = ConnectionClosed; // Just tear it down, the peer rejected the connection.
		break;
	case ConnectionOK:
		connectionState = ConnectionPeerClosed;
		break;
	case ConnectionDisconnecting:
		connectionState = ConnectionClosed;
		break;
	case ConnectionPeerClosed:
	case ConnectionClosed:
		break; // We've already in the state where peer has closed the connection, no need to do anything.
	default:
		LOG(LogError, "SetPeerClosed() called at an unexpected time. The internal connectionState has an invalid value %d!", (int)connectionState); 
		break;
	}
}

void MessageConnection::FreeMessageData() // [main thread]
{
	assert(!IsWorkerThreadRunning());

	while(outboundAcceptQueue.Size() > 0)
	{
		NetworkMessage *msg = outboundAcceptQueue.TakeFront();
		delete msg;
	}

	while(inboundMessageQueue.Size() > 0)
	{
		NetworkMessage *msg = inboundMessageQueue.TakeFront();
		delete msg;
	}

//	for(int i = 0; i < outboundQueue.Size(); ++i) // For MaxHeap
//		delete outboundQueue.data[i];
	for(unsigned long i = 0; i < outboundQueue.Size(); ++i)
		delete *outboundQueue.ItemAt(i);

	outboundQueue.Clear();

	inboundContentIDStamps.clear();

	outboundContentIDMessages.clear();

	Lockable<FragmentedSendManager>::LockType sends = fragmentedSends.Acquire();
	sends->FreeAllTransfers();

	fragmentedReceives.transfers.clear();

	Lockable<ConnectionStatistics>::LockType stats_ = statistics.Acquire();
	stats_->ping.clear();
	stats_->recvPacketIDs.clear();
	stats_->traffic.clear();
}

void MessageConnection::DetectConnectionTimeOut()
{
	AssertInWorkerThreadContext();

	if (connectionState == ConnectionClosed)
		return;

	float lastHeardSince = LastHeardTime();
	if (lastHeardSince > connectionLostTimeout)
	{
		LOG(LogInfo, "It's been %.2fms since last heard from other end. connectionLostTimeout=%.2fms, so closing connection.",
			lastHeardSince, connectionLostTimeout);
		connectionState = ConnectionClosed;
	}
}

void MessageConnection::AcceptOutboundMessages() // [worker thread]
{
	AssertInWorkerThreadContext();

	if (connectionState != ConnectionOK)
		return;

//	assert(ContainerUniqueAndNoNullElements(outboundAcceptQueue));

	// To throttle an over-eager main application, only accept this many messages from the main thread
	// at each execution frame.
	int numMessagesToAcceptPerFrame = 500;

	// Empty the queue from messages that the main thread has submitted for sending.
	while(outboundAcceptQueue.Size() > 0 && --numMessagesToAcceptPerFrame > 0)
	{
		assert(outboundAcceptQueue.Front() != 0);
		NetworkMessage *msg = *outboundAcceptQueue.Front();
		outboundAcceptQueue.PopFront();

		outboundQueue.InsertWithResize(msg);
		CheckAndSaveOutboundMessageWithContentID(msg);
	}
	assert(ContainerUniqueAndNoNullElements(outboundQueue));
//	assert(ContainerUniqueAndNoNullElements(outboundAcceptQueue));
}

void MessageConnection::UpdateConnection() // [Called from the worker thread]
{
	AssertInWorkerThreadContext();

	if (!socket)
		return;

	AcceptOutboundMessages();

	// MessageConnection needs to automatically manage the sending of ping messages in an unreliable channel.
	if (connectionState == ConnectionOK && pingTimer.TriggeredOrNotRunning())
	{
		if (!bOutboundSendsPaused)
			SendPingRequestMessage();
		DetectConnectionTimeOut();
		pingTimer.StartMSecs(pingIntervalMSecs);
	}

	// Produce statistics back to the application about the current connection state.
	if (statsRefreshTimer.TriggeredOrNotRunning())
	{
		ComputeStats();
		statsRefreshTimer.StartMSecs(statsRefreshIntervalMSecs);

		// Check if the socket is dead and mark it read-closed.
		if ((connectionState == ConnectionOK || connectionState == ConnectionDisconnecting) && IsReadOpen())
			if (!socket || !socket->IsReadOpen())
			{
				LOG(LogInfo, "Peer closed connection.");
				SetPeerClosed();
			}
	}

	// Perform the TCP/UDP -specific connection update.
	DoUpdateConnection();
}

NetworkMessage *MessageConnection::AllocateNewMessage()
{
	NetworkMessage *msg = messagePool.New();
	LOG(LogObjectAlloc, "MessageConnection::AllocateMessage %p!", msg);
	return msg;
}

void MessageConnection::FreeMessage(NetworkMessage *msg)
{
	if (!msg)
		return;

	LOG(LogObjectAlloc, "MessageConnection::FreeMessage %p!", msg);
	messagePool.Free(msg);
}

NetworkMessage *MessageConnection::StartNewMessage(unsigned long id, size_t numBytes)
{
	NetworkMessage *msg = AllocateNewMessage();
	if (!msg)
	{
		LOG(LogError, "MessageConnection::SendMessage: StartNewMessage failed! Discarding message send.");
		return 0; // Failed to allocate a new message. This is caused only by memory allocation issues.
	}

	msg->id = id;
	msg->reliable = false;
	msg->contentID = 0;
	msg->obsolete = false;

	// Give the new message the lowest priority by default.
	msg->priority = 0;

	// By default, the message is not fragmented. Later when admitting the message into the send queue, the need for
	// fragmentation is examined and this field will be updated if needed.
	msg->transfer = 0; 

	msg->Resize(numBytes);

	return msg;
}

void MessageConnection::SplitAndQueueMessage(NetworkMessage *message, bool internalQueue, size_t maxFragmentSize)
{
	using namespace std;

	assert(message);
	assert(!message->obsolete);

	// We need this many fragments to represent the whole message.
	const size_t totalNumFragments = (message->dataSize + maxFragmentSize - 1) / maxFragmentSize;
	assert(totalNumFragments > 1); // Shouldn't be calling this function if the message can well fit into one fragment.

	LOG(LogVerbose, "Splitting a message of %db into %d fragments of %db size at most.",
		(int)message->dataSize, (int)totalNumFragments, (int)maxFragmentSize);

/** \todo Would like to do this:
	FragmentedSendManager::FragmentedTransfer *transfer;
	{
		Lock<FragmentedSendManager> sends = fragmentedSends.Acquire();
		transfer = sends->AllocateNewFragmentedTransfer();
	}
*/
	// But instead, have to resort to function-wide lock.
	Lock<FragmentedSendManager> sends = fragmentedSends.Acquire();
	FragmentedSendManager::FragmentedTransfer *transfer = sends->AllocateNewFragmentedTransfer();

	size_t currentFragmentIndex = 0;
	size_t byteOffset = 0;

	assert(transfer != 0);
	transfer->totalNumFragments = totalNumFragments;

	if (!message->reliable)
	{
		LOG(LogVerbose, "Upgraded a nonreliable message with ID %d and size %d to a reliable message since it had to be fragmented!", (int)message->id, (int)message->dataSize);
	}

	// Split the message into fragments.
	while(byteOffset < message->dataSize)
	{
		const size_t thisFragmentSize = min(maxFragmentSize, message->dataSize - byteOffset);

		NetworkMessage *fragment = StartNewMessage(message->id, thisFragmentSize);
		fragment->contentID = message->contentID;
		fragment->inOrder = message->inOrder;
		fragment->reliable = true; // We don't send fragmented messages as unreliable messages - the risk of a fragment getting lost wastes bandwidth.
		fragment->messageNumber = outboundMessageNumberCounter++; ///\todo Convert to atomic increment, or this is a race condition.
		fragment->reliableMessageNumber = message->reliableMessageNumber;
		fragment->priority = message->priority;
		fragment->sendCount = 0;

		fragment->transfer = transfer;
		fragment->fragmentIndex = currentFragmentIndex++;

		// Copy the data from the old message that's supposed to go into this fragment.
		memcpy(fragment->data, message->data + byteOffset, thisFragmentSize);
		byteOffset += thisFragmentSize;

		transfer->AddMessage(fragment);

		if (internalQueue) // if true, we are accessing from the worker thread, and can directly access the outboundQueue member.
		{
			assert(ContainerUniqueAndNoNullElements(outboundQueue));
			outboundQueue.InsertWithResize(fragment);
			assert(ContainerUniqueAndNoNullElements(outboundQueue));
		}
		else
		{
			if (!outboundAcceptQueue.Insert(fragment))
			{
				///\todo Is it possible to check beforehand if this criteria is avoided, or if we are doomed?
				LOG(LogError, "Critical: Failed to add message fragment to outboundAcceptQueue! Queue was full. Do not know how to recover here!");
				assert(false);
			}
		}
	}

	// Signal the worker thread that there are new outbound events available.
	if (!bOutboundSendsPaused)
		eventMsgsOutAvailable.Set();

	// The original message that was split into fragments is no longer needed - it is represented by the newly created fragments
	// that have now been queued.
	FreeMessage(message);
}

void MessageConnection::EndAndQueueMessage(NetworkMessage *msg, size_t numBytes, bool internalQueue)
{
	assert(msg);
	if (!msg)
		return;

	// If the message was marked obsolete to start with, discard it.
	if (msg->obsolete || !socket || GetConnectionState() == ConnectionClosed || !socket->IsWriteOpen() || 
		(internalQueue == false && !IsWriteOpen()))
	{
		LOG(LogVerbose, "MessageConnection::EndAndQueueMessage: Discarded message with ID 0x%X and size %d bytes. "
			"msg->obsolete: %d. socket ptr: %p. ConnectionState: %s. socket->IsWriteOpen(): %s. msgconn->IsWriteOpen: %s. "
			"internalQueue: %s.",
			(int)msg->id, (int)numBytes, (int)msg->obsolete, socket, ConnectionStateToString(GetConnectionState()).c_str(), (socket && socket->IsWriteOpen()) ? "true" : "false",
			IsWriteOpen() ? "true" : "false", internalQueue ? "true" : "false");
		FreeMessage(msg);
		return;
	}

	// Remember the amount of bytes the client said to be using for later.
	if (numBytes != (size_t)(-1))
		msg->dataSize = numBytes;

	assert(msg->dataSize <= msg->Capacity());
	if (msg->dataSize > msg->Capacity())
	{
		LOG(LogError, "Critical! User specified a larger NetworkMessage than there is Capacity() for. Call NetworkMessage::Reserve() "
			"to ensure there is a proper amount of space for the buffer! Specified: %d bytes, Capacity(): %d bytes.",
			(int)msg->dataSize, (int)msg->Capacity());
	}

	// Check if the message is too big - in that case we split it into fixed size fragments and add them into the queue.
	///\todo We can optimize here by doing the splitting at datagram creation time to create optimally sized datagrams, but
	/// it is quite more complicated, so left for later. 
	const size_t sendHeaderUpperBound = 32; // Reserve some bytes for the packet and message headers. (an approximate upper bound)
	if (msg->dataSize + sendHeaderUpperBound > socket->MaxSendSize())
	{
		const size_t maxFragmentSize = socket->MaxSendSize() / 4 - sendHeaderUpperBound; ///\todo Check this is ok.
		assert(maxFragmentSize > 0 && maxFragmentSize < socket->MaxSendSize());
		SplitAndQueueMessage(msg, internalQueue, maxFragmentSize);
		return;
	}

	msg->messageNumber = outboundMessageNumberCounter++; ///\todo Convert to atomic increment, or this is a race condition.
	msg->reliableMessageNumber = (msg->reliable ? outboundReliableMessageNumberCounter++ : 0); ///\todo Convert to atomic increment, or this is a race condition.
	msg->sendCount = 0;

	if (internalQueue) // if true, we are accessing from the worker thread, and can directly access the outboundQueue member.
	{
		LOG(LogVerbose, "MessageConnection::EndAndQueueMessage: Internal-queued message of size %d bytes and ID 0x%X.", (int)msg->Size(), (int)msg->id);
		assert(ContainerUniqueAndNoNullElements(outboundQueue));
		outboundQueue.InsertWithResize(msg);
		assert(ContainerUniqueAndNoNullElements(outboundQueue));
	}
	else
	{
		if (!outboundAcceptQueue.Insert(msg))
		{
			if (msg->reliable) // For nonreliable messages it is not critical if we can't enqueue the message. Just discard it.
			{
				///\todo Is it possible to check beforehand if this criteria is avoided, or if we are doomed?
				LOG(LogVerbose, "Critical: Failed to add new reliable message to outboundAcceptQueue! Queue was full. Discarding the message!");
				assert(false);
			}
			FreeMessage(msg);
			return;
		}
		LOG(LogData, "MessageConnection::EndAndQueueMessage: Queued message of size %d bytes and ID 0x%X.", (int)msg->Size(), (int)msg->id);
	}

	// Signal the worker thread that there are new outbound events available.
	if (!bOutboundSendsPaused)
		eventMsgsOutAvailable.Set();
}

void MessageConnection::SendMessage(unsigned long id, bool reliable, bool inOrder, unsigned long priority, 
                                    unsigned long contentID, const char *data, size_t numBytes)
{
	AssertInMainThreadContext();

	NetworkMessage *msg = StartNewMessage(id, numBytes);
	if (!msg)
	{
		LOG(LogError, "MessageConnection::SendMessage: StartNewMessage failed! Discarding message send.");
		return;
	}
	msg->reliable = reliable;
	msg->inOrder = inOrder;
	msg->priority = priority;
	msg->contentID = contentID;
	assert(msg->data);
	assert(msg->Size() == numBytes);
	memcpy(msg->data, data, numBytes);
	EndAndQueueMessage(msg);
}

/// Called from the main thread to fetch & handle all new inbound messages.
void MessageConnection::Process(int maxMessagesToProcess)
{
	AssertInMainThreadContext();

	assert(maxMessagesToProcess >= 0);

	// Check the status of the connection worker thread.
	if (connectionState == ConnectionClosed || !socket || !socket->Connected())
	{
		if (socket)
			Close(); ///\todo This will block, since it is called with the default time period.
		connectionState = ConnectionClosed;
		return;
	}

	// The number of messages we are willing to process this cycle. If there are fewer messages than this 
	// to process, we will return immediately (won't wait for this many messages to actually be received, it is just an upper limit).
	int numMessagesLeftToProcess = maxMessagesToProcess;

	while(inboundMessageQueue.Size() > 0 && (numMessagesLeftToProcess-- > 0 || maxMessagesToProcess == 0))
	{
		if (!inboundMessageHandler)
		{
			LOG(LogVerbose, "Warning! Cannot process messages since no message handler registered to connection %s!",
				ToString().c_str());
			return;
		}

		NetworkMessage **message = inboundMessageQueue.Front();
		assert(message);
		NetworkMessage *msg = *message;
		inboundMessageQueue.PopFront();
		assert(msg);

		inboundMessageHandler->HandleMessage(this, msg->id, (msg->dataSize > 0) ? msg->data : 0, msg->dataSize);

		FreeMessage(msg);
	}
}

void MessageConnection::WaitForMessage(int maxMSecsToWait) // [main thread]
{
	AssertInMainThreadContext();

	// If we have a message to process, no need to wait.
	if (inboundMessageQueue.Size() > 0)
		return;

	// Check the status of the connection worker thread.
	if (connectionState == ConnectionClosed)
	{
		if (socket)
			Close();
		return;
	}

	// Wait indefinitely until we get a new message, or the connection is torn down.
	if (maxMSecsToWait == 0)
	{
		///\todo Log out warning if this takes AGES. Or rather, perhaps remove support for this altogether
		/// to avoid deadlocks.
		while(inboundMessageQueue.Size() == 0 && GetConnectionState() == ConnectionOK)
			Clock::Sleep(1); ///\todo Instead of waiting multiple 1msec slices, should wait for proper event.
	}
	else
	{
		PolledTimer timer;
		timer.StartMSecs((float)maxMSecsToWait);
		while(inboundMessageQueue.Size() == 0 && GetConnectionState() == ConnectionOK && !timer.Test())
			Clock::Sleep(1); ///\todo Instead of waiting multiple 1msec slices, should wait for proper event.

		if (timer.MSecsElapsed() >= 1000.f)
		{
				LOG(LogWaits, "MessageConnection::WaitForMessage: Waited %f msecs for a new message. ConnectionState: %s. %d messages in queue.",
				timer.MSecsElapsed(), ConnectionStateToString(GetConnectionState()).c_str(), (int)inboundMessageQueue.Size());
		}
	}
}

NetworkMessage *MessageConnection::ReceiveMessage(int maxMSecsToWait) // [main thread]
{
	AssertInMainThreadContext();

	// Check the status of the connection worker thread.
	if (connectionState == ConnectionClosed)
	{
		if (socket)
			Close();
		return 0;
	}

	// If we don't have a message, wait for the given duration to receive one.
	if (inboundMessageQueue.Size() == 0 && maxMSecsToWait >= 0)
		WaitForMessage(maxMSecsToWait);

	// Did we get a message even after the max timeout?
	if (inboundMessageQueue.Size() == 0)
		return 0;

	NetworkMessage *message = *inboundMessageQueue.Front();
	inboundMessageQueue.PopFront();
	assert(message);

	return message;
}

bool EraseReliableIfObsoleteOrNotInOrderCmp(const NetworkMessage *msg)
{
	assert(msg->reliable);
	return msg->inOrder == false || msg->obsolete;
}

bool EraseReliableIfObsoleteCmp(const NetworkMessage *msg)
{
	assert(msg->reliable);
	return msg->obsolete;
}

int NetworkMessage::GetTotalDatagramPackedSize() const
{
//	const int idLength = (transfer == 0 || fragmentIndex == 0) ? VLE8_16_32::GetEncodedBitLength(id)/8 : 0;
//	const int headerLength = 2;
	const int headerLength = 30; ///\todo This is loose, but since it only needs to be an upper bound, it is safe now.
	const int contentLength = dataSize;
	return headerLength + contentLength;
//	const int fragmentStartLength = (transfer && fragmentIndex == 0) ? VLE8_16_32::GetEncodedBitLength(transfer->totalNumFragments)/8 : 0;
//	const int fragmentLength = (transfer ? 1 : 0) + ((transfer && fragmentIndex != 0) ? VLE8_16_32::GetEncodedBitLength(fragmentIndex)/8 : 0);

	///\todo Take into account the inOrder field.
//	return idLength + headerLength + contentLength + fragmentStartLength + fragmentLength;
}

void MessageConnection::AddOutboundStats(unsigned long numBytes, unsigned long numPackets, unsigned long numMessages)
{
	AssertInWorkerThreadContext();

	if (numBytes == 0 && numMessages == 0 && numPackets == 0)
		return;

	ConnectionStatistics &cs = statistics.LockGet();
	cs.traffic.push_back(ConnectionStatistics::TrafficTrack());
	ConnectionStatistics::TrafficTrack &t = cs.traffic.back();
	t.bytesIn = t.messagesIn = t.packetsIn = 0;
	t.bytesOut = numBytes;
	t.packetsOut = numPackets;
	t.messagesOut = numMessages;
	t.tick = Clock::Tick();
	statistics.Unlock();
}

void MessageConnection::AddInboundStats(unsigned long numBytes, unsigned long numPackets, unsigned long numMessages)
{
	AssertInWorkerThreadContext();

	if (numBytes == 0 && numMessages == 0 && numPackets == 0)
		return;

	ConnectionStatistics &cs = statistics.LockGet();
	cs.traffic.push_back(ConnectionStatistics::TrafficTrack());
	ConnectionStatistics::TrafficTrack &t = cs.traffic.back();
	t.bytesOut = t.messagesOut = t.packetsOut = 0;
	t.bytesIn = numBytes;
	t.packetsIn = numPackets;
	t.messagesIn = numMessages;
	t.tick = Clock::Tick();
	statistics.Unlock();
}

void MessageConnection::ComputeStats()
{
	AssertInWorkerThreadContext();

	ConnectionStatistics &cs = statistics.LockGet();

	const tick_t maxEntryAge = Clock::TicksPerSec() * 5;
	const tick_t timeNow = Clock::Tick();
	const tick_t maxTickAge = timeNow - maxEntryAge;

	for(size_t i = 0; i < cs.traffic.size(); ++i)
		if (Clock::IsNewer(cs.traffic[i].tick, maxTickAge))
		{
			cs.traffic.erase(cs.traffic.begin(), cs.traffic.begin() + i);
			break;
		}

	if (cs.traffic.size() <= 1)
	{
		bytesInPerSec = bytesOutPerSec = msgsInPerSec = msgsOutPerSec = packetsInPerSec = packetsOutPerSec = 0.f;
		statistics.Unlock();
		return;
	}

	unsigned long totalBytesIn = 0;
	unsigned long totalBytesOut = 0;
	unsigned long totalMsgsIn = 0;
	unsigned long totalMsgsOut = 0;
	unsigned long totalPacketsIn = 0;
	unsigned long totalPacketsOut = 0;

	for(size_t i = 0; i < cs.traffic.size(); ++i)
	{
		totalBytesIn += cs.traffic[i].bytesIn;
		totalBytesOut += cs.traffic[i].bytesOut;
		totalPacketsIn += cs.traffic[i].packetsIn;
		totalPacketsOut += cs.traffic[i].packetsOut;
		totalMsgsIn += cs.traffic[i].messagesIn;
		totalMsgsOut += cs.traffic[i].messagesOut;
	}
	tick_t ticks = cs.traffic.back().tick - cs.traffic.front().tick;
	float secs = max(1.f, (float)Clock::TicksToMillisecondsD(ticks) / 1000.f);
	bytesInPerSec = (float)totalBytesIn / secs;
	bytesOutPerSec = (float)totalBytesOut / secs;
	packetsInPerSec = (float)totalPacketsIn / secs;
	packetsOutPerSec = (float)totalPacketsOut / secs;
	msgsInPerSec = (float)totalMsgsIn / secs;
	msgsOutPerSec = (float)totalMsgsOut / secs;

	statistics.Unlock();
}

void MessageConnection::CheckAndSaveOutboundMessageWithContentID(NetworkMessage *msg)
{
	AssertInWorkerThreadContext();
	assert(msg);

	if (msg->contentID == 0)
		return;

	MsgContentIDPair key = std::make_pair(msg->id, msg->contentID);
	ContentIDSendTrack::iterator iter = outboundContentIDMessages.find(key);
	if (iter != outboundContentIDMessages.end())
	{
		if (msg->IsNewerThan(*iter->second))
		{
			iter->second->obsolete = true;

			assert(iter->second != msg);
			assert(iter->first.first == iter->second->id);
			assert(iter->first.second == iter->second->contentID);
			assert(iter->first.first == msg->id);
			assert(iter->first.second == msg->contentID);
			iter->second = msg;
		}
		else
		{
			LOG(LogError, "Warning! Adding new message ID %d, number %d, content ID %d, priority %d, but it was obsoleted by an already existing message number %d.", 
				(int)msg->id, (int)msg->messageNumber, (int)msg->contentID, (int)iter->second->priority, (int)iter->second->messageNumber);
			msg->obsolete = true;
		}
	}
	else
	{
		outboundContentIDMessages[key] = msg;
	}
}

void MessageConnection::ClearOutboundMessageWithContentID(NetworkMessage *msg)
{
	AssertInWorkerThreadContext();

	///\bug Possible race condition here. Accessed by both main and worker thread through a call from FreeMessage.
	assert(msg);
	if (msg->contentID == 0)
		return;
	MsgContentIDPair key = std::make_pair(msg->id, msg->contentID);
	ContentIDSendTrack::iterator iter = outboundContentIDMessages.find(key);
	if (iter != outboundContentIDMessages.end())
		if (msg == iter->second)
			outboundContentIDMessages.erase(iter);
}

bool MessageConnection::CheckAndSaveContentIDStamp(u32 messageID, u32 contentID, packet_id_t packetID)
{
	AssertInWorkerThreadContext();

	assert(contentID != 0);

	tick_t now = Clock::Tick();

	MsgContentIDPair key = std::make_pair(messageID, contentID);
	ContentIDReceiveTrack::iterator iter = inboundContentIDStamps.find(key);
	if (iter == inboundContentIDStamps.end())
	{
		inboundContentIDStamps[key] = std::make_pair(packetID, now);
		return true;
	}
	else
	{
		if (PacketIDIsNewerThan(packetID, iter->second.first) || (float)Clock::TimespanToMillisecondsD(iter->second.second, now) > 5.f * 1000.f)
		{
			iter->second = std::make_pair(packetID, now);
			return true;
		}
		else
			return false;
	}
}

void MessageConnection::HandleInboundMessage(packet_id_t packetID, const char *data, size_t numBytes)
{
	AssertInWorkerThreadContext();

	assert(data && numBytes > 0);
	assert(socket);

	// Read the message ID.
	DataDeserializer reader(data, numBytes);
	u32 messageID = reader.ReadVLE<VLE8_16_32>(); ///\todo Check that there actually is enough space to read.
	if (messageID == DataDeserializer::VLEReadError)
	{
		LOG(LogError, "Error parsing messageID of a message in socket %s. Data size: %d bytes.", socket->ToString().c_str(), (int)numBytes);
		///\todo Should kill/Close the connection right here and now?
		return;
	}
	LOG(LogData, "Received message with ID %d and size %d from peer %s.", (int)packetID, (int)numBytes, socket->ToString().c_str());

	// Pass the message to TCP/UDP -specific message handler.
	bool childHandledMessage = HandleMessage(packetID, messageID, data + reader.BytePos(), reader.BytesLeft());
	if (childHandledMessage)
		return; // If the derived class handled the message, no need to propagate it further.

	switch(messageID)
	{
	case MsgIdPingRequest:
		HandlePingRequestMessage(data + reader.BytePos(), reader.BytesLeft());
		break;
	case MsgIdPingReply:
		HandlePingReplyMessage(data + reader.BytePos(), reader.BytesLeft());
		break;
	default:
		{
			NetworkMessage *msg = AllocateNewMessage();
			msg->Resize(numBytes);
			assert(reader.BitPos() == 0);
			memcpy(msg->data, data + reader.BytePos(), reader.BytesLeft());
			msg->dataSize = reader.BytesLeft();
			msg->id = messageID;
			msg->contentID = 0;
			bool success = inboundMessageQueue.Insert(msg);
			if (!success)
			{
				LOG(LogError, "Failed to add a new message of ID %d and size %dB to inbound queue! Queue was full.",
					(int)messageID, (int)msg->dataSize);
				FreeMessage(msg);
			}
		}
		break;
	}
}

void MessageConnection::SetMaximumDataSendRate(int numBytesPerSec, int numDatagramsPerSec)
{
}

void MessageConnection::RegisterInboundMessageHandler(IMessageHandler *handler)
{ 
	AssertInMainThreadContext();

	inboundMessageHandler = handler;
}

void MessageConnection::SendPingRequestMessage()
{
	AssertInWorkerThreadContext();

	ConnectionStatistics &cs = statistics.LockGet();
	
	u8 pingID = (u8)((cs.ping.size() == 0) ? 1 : (cs.ping.back().pingID + 1));
	cs.ping.push_back(ConnectionStatistics::PingTrack());
	ConnectionStatistics::PingTrack &pingTrack = cs.ping.back();
	pingTrack.replyReceived = false;
	pingTrack.pingSentTick = Clock::Tick();
	pingTrack.pingID = pingID;

	statistics.Unlock();

	NetworkMessage *msg = StartNewMessage(MsgIdPingRequest, 1);
	msg->data[0] = pingID;
	msg->priority = NetworkMessage::cMaxPriority - 2;
	EndAndQueueMessage(msg, 1, true);
	LOG(LogVerbose, "Enqueued ping message %d.", (int)pingID);
}

void MessageConnection::HandlePingRequestMessage(const char *data, size_t numBytes)
{
	AssertInWorkerThreadContext();

	if (numBytes != 1)
	{
		LOG(LogError, "Malformed PingRequest message received! Size was %d bytes, expected 1 byte!", (int)numBytes);
		return;
	}

	u8 pingID = (u8)*data;
	NetworkMessage *msg = StartNewMessage(MsgIdPingReply, 1);
	msg->data[0] = pingID;
	msg->priority = NetworkMessage::cMaxPriority - 1;
	EndAndQueueMessage(msg, 1, true);
	LOG(LogVerbose, "HandlePingRequestMessage: %d.", (int)pingID);
}

void MessageConnection::HandlePingReplyMessage(const char *data, size_t numBytes)
{
	AssertInWorkerThreadContext();

	if (numBytes != 1)
	{
		LOG(LogError, "Malformed PingReply message received! Size was %d bytes, expected 1 byte!", (int)numBytes);
		return;
	}

	ConnectionStatistics &cs = statistics.LockGet();

	// How much to bias the new rtt value against the old rtt estimation. 1.f - 100% biased to the new value. near zero - very stable and nonfluctuant.
	const float rttPredictBias = 0.5f;

	u8 pingID = *(u8*)data;
	for(size_t i = 0; i < cs.ping.size(); ++i)
		if (cs.ping[i].pingID == pingID && cs.ping[i].replyReceived == false)
		{
			cs.ping[i].pingReplyTick = Clock::Tick();
			float newRtt = (float)Clock::TicksToMillisecondsD(Clock::TicksInBetween(cs.ping[i].pingReplyTick, cs.ping[i].pingSentTick));
			cs.ping[i].replyReceived = true;
			statistics.Unlock();
			rtt = rttPredictBias * newRtt + (1.f * rttPredictBias) * rtt;

			LOG(LogVerbose, "HandlePingReplyMessage: %d.", (int)pingID);
			return;
		}

	statistics.Unlock();
	LOG(LogError, "Received PingReply with ID %d in socket %s, but no matching PingRequest was ever sent!", (int)pingID, socket->ToString().c_str());
}

std::string MessageConnection::ToString() const
{
	if (socket)
		return socket->ToString();
	else
		return "(Not connected)";
}

void MessageConnection::DumpStatus() const
{
	AssertInMainThreadContext();

	char str[4096];

	sprintf(str, "Connection Status: %s.\n"
		"\tOutboundMessagesPending: %d.\n"
		"\tMessageConnection: %s %s %s.\n"
		"\tSocket: %s %s %s %s.\n"
		"\tRound-Trip Time: %.2fms.\n"
		"\tLatency: %.2fms.\n"
		"\tLastHeardTime: %.2fms.\n"
		"\tDatagrams in: %.2f/sec.\n"
		"\tDatagrams out: %.2f/sec.\n"
		"\tMessages in: %.2f/sec.\n"
		"\tMessages out: %.2f/sec.\n"
		"\tBytes in: %s/sec.\n"
		"\tBytes out: %s/sec.\n"
		"\tEventMsgsOutAvailable: %d.\n"
		"\tOverlapped in: %d (event: %s)\n"
		"\tOverlapped out: %d (event: %s)\n"
		"\tTime until next send: %d\n",
		ConnectionStateToString(GetConnectionState()).c_str(),
		(int)NumOutboundMessagesPending(),
		Connected() ? "connected" : "",
		IsReadOpen() ? "readOpen" : "",
		IsWriteOpen() ? "writeOpen" : "",
		socket ? "exists" : "zero",
		(socket && socket->Connected()) ? "connected" : "",
		(socket && socket->IsReadOpen()) ? "readOpen" : "",
		(socket && socket->IsWriteOpen()) ? "writeOpen" : "",
		RoundTripTime(),
		Latency(), LastHeardTime(), PacketsInPerSec(), PacketsOutPerSec(),
		MsgsInPerSec(), MsgsOutPerSec(), 
		FormatBytes((size_t)BytesInPerSec()).c_str(), FormatBytes((size_t)BytesOutPerSec()).c_str(),
		(int)eventMsgsOutAvailable.Test(), 
#ifdef WIN32
		socket ? socket->NumOverlappedReceivesInProgress() : -1,
#else
		-1,
#endif
		(socket && socket->GetOverlappedReceiveEvent().Test()) ? "true" : "false",
#ifdef WIN32
		socket ? socket->NumOverlappedSendsInProgress() : -1,
#else
		-1,
#endif
		(socket && socket->GetOverlappedSendEvent().Test()) ? "true" : "false",
		(int)TimeUntilCanSendPacket());

	LOGUSER(str);

	DumpConnectionStatus();
}

Event MessageConnection::NewOutboundMessagesEvent() const
{
	assert(!eventMsgsOutAvailable.IsNull());

	return eventMsgsOutAvailable;
}

MessageConnection::SocketReadResult MessageConnection::ReadSocket()
{ 
	AssertInWorkerThreadContext();

	size_t ignored = 0; 
	return ReadSocket(ignored); 
}

void MessageConnection::AssertInWorkerThreadContext() const
{
#ifdef THREAD_CHECKING_ENABLED
	bool ret = workerThread != 0 && Thread::CurrentThreadId() == workerThreadId;
	assert(ret);
#endif
}

void MessageConnection::AssertInMainThreadContext() const
{
#ifdef THREAD_CHECKING_ENABLED
//	bool ret = !InWorkerThreadContext();
	bool ret = workerThread == 0 || Thread::CurrentThreadId() != workerThreadId;
	assert(ret);
#endif
}

void MessageConnection::SetWorkerThread(NetworkWorkerThread *thread)
{
	workerThread = thread;
#ifdef THREAD_CHECKING_ENABLED
	workerThreadId = thread ? thread->ThreadObject().Id() : Thread::NullThreadId();
#endif
	
	AssertInMainThreadContext();
}

EndPoint MessageConnection::LocalEndPoint() const
{
	if (socket)
		return socket->LocalEndPoint();
	else
		return EndPoint();
}

EndPoint MessageConnection::RemoteEndPoint() const
{
	if (socket)
		return socket->RemoteEndPoint();
	else
		return EndPoint();
}

} // ~kNet
