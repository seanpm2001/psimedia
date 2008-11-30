/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "rwcontrol.h"

#include <QPointer>
#include "gstthread.h"
#include "rtpworker.h"

// note: queuing frames doesn't really make much sense, since if the UI
//   receives 5 frames at once, they'll just get painted on each other in
//   succession and you'd only really see the last one.  however, we'll queue
//   frames in case we ever want to do timestamped frames.
#define QUEUE_FRAME_MAX 10

namespace PsiMedia {

static int queuedFrameInfo(const QList<RwControlMessage*> &list, RwControlFrame::Type type, int *firstPos)
{
	int count = 0;
	bool first = true;
	for(int n = 0; n < list.count(); ++n)
	{
		const RwControlMessage *msg = list[n];
		if(msg->type == RwControlMessage::Frame && ((RwControlFrameMessage *)msg)->frame.type == type)
		{
			if(first)
				*firstPos = n;
			++count;
			first = false;
		}
	}
	return count;
}

static RwControlFrameMessage *getLatestFrameAndRemoveOthers(QList<RwControlMessage*> *list, RwControlFrame::Type type)
{
	RwControlFrameMessage *fmsg = 0;
	for(int n = 0; n < list->count(); ++n)
	{
		RwControlMessage *msg = list->at(n);
		if(msg->type == RwControlMessage::Frame && ((RwControlFrameMessage *)msg)->frame.type == type)
		{
			// if we already had a frame, discard it and take the next
			if(fmsg)
				delete fmsg;

			fmsg = (RwControlFrameMessage *)msg;
			list->removeAt(n);
			--n; // adjust position
		}
	}
	return fmsg;
}

static RwControlAudioIntensityMessage *getLatestAudioIntensityAndRemoveOthers(QList<RwControlMessage*> *list)
{
	RwControlAudioIntensityMessage *amsg = 0;
	for(int n = 0; n < list->count(); ++n)
	{
		RwControlMessage *msg = list->at(n);
		if(msg->type == RwControlMessage::AudioIntensity)
		{
			// if we already had a msg, discard it and take the next
			if(amsg)
				delete amsg;

			amsg = (RwControlAudioIntensityMessage *)msg;
			list->removeAt(n);
			--n; // adjust position
		}
	}
	return amsg;
}

static RwControlStatusMessage *statusFromWorker(RtpWorker *worker)
{
	RwControlStatusMessage *msg = new RwControlStatusMessage;
	msg->status.localAudioParams = worker->localAudioParams;
	msg->status.localVideoParams = worker->localVideoParams;
	msg->status.localAudioPayloadInfo = worker->localAudioPayloadInfo;
	msg->status.localVideoPayloadInfo = worker->localVideoPayloadInfo;
	msg->status.canTransmitAudio = worker->canTransmitAudio;
	msg->status.canTransmitVideo = worker->canTransmitVideo;
	return msg;
}

static void applyDevicesToWorker(RtpWorker *worker, const RwControlConfigDevices &devices)
{
	worker->aout = devices.audioOutId;
	worker->ain = devices.audioInId;
	worker->vin = devices.videoInId;
	worker->infile = devices.fileNameIn;
	worker->indata = devices.fileDataIn;
	worker->loopFile = devices.loopFile;
	worker->setOutputVolume(devices.audioOutVolume);
	worker->setInputVolume(devices.audioInVolume);
}

static void applyCodecsToWorker(RtpWorker *worker, const RwControlConfigCodecs &codecs)
{
	if(codecs.useLocalAudioParams)
		worker->localAudioParams = codecs.localAudioParams;
	if(codecs.useLocalVideoParams)
		worker->localVideoParams = codecs.localVideoParams;
	if(codecs.useLocalAudioPayloadInfo)
		worker->localAudioPayloadInfo = codecs.localAudioPayloadInfo;
	if(codecs.useLocalVideoPayloadInfo)
		worker->localVideoPayloadInfo = codecs.localVideoPayloadInfo;
	if(codecs.useRemoteAudioPayloadInfo)
		worker->remoteAudioPayloadInfo = codecs.remoteAudioPayloadInfo;
	if(codecs.useRemoteVideoPayloadInfo)
		worker->remoteVideoPayloadInfo = codecs.remoteVideoPayloadInfo;
}

//----------------------------------------------------------------------------
// RwControlLocal
//----------------------------------------------------------------------------
RwControlLocal::RwControlLocal(GstThread *thread, QObject *parent) :
	QObject(parent),
	app(0),
	cb_rtpAudioOut(0),
	cb_rtpVideoOut(0),
	cb_recordData(0),
	wake_pending(false)
{
	thread_ = thread;
	remote_ = 0;

	// create RwControlRemote, block until ready
	QMutexLocker locker(&m);
	timer = g_timeout_source_new(0);
	g_source_set_callback(timer, cb_doCreateRemote, this, NULL);
	g_source_attach(timer, thread_->mainContext());
	w.wait(&m);
}

RwControlLocal::~RwControlLocal()
{
	// delete RwControlRemote, block until done
	QMutexLocker locker(&m);
	timer = g_timeout_source_new(0);
	g_source_set_callback(timer, cb_doDestroyRemote, this, NULL);
	g_source_attach(timer, thread_->mainContext());
	w.wait(&m);

	qDeleteAll(in);
}

void RwControlLocal::start(const RwControlConfigDevices &devices, const RwControlConfigCodecs &codecs)
{
	RwControlStartMessage *msg = new RwControlStartMessage;
	msg->devices = devices;
	msg->codecs = codecs;
	remote_->postMessage(msg);
}

void RwControlLocal::stop()
{
	RwControlStopMessage *msg = new RwControlStopMessage;
	remote_->postMessage(msg);
}

void RwControlLocal::updateDevices(const RwControlConfigDevices &devices)
{
	RwControlUpdateDevicesMessage *msg = new RwControlUpdateDevicesMessage;
	msg->devices = devices;
	remote_->postMessage(msg);
}

void RwControlLocal::updateCodecs(const RwControlConfigCodecs &codecs)
{
	RwControlUpdateCodecsMessage *msg = new RwControlUpdateCodecsMessage;
	msg->codecs = codecs;
	remote_->postMessage(msg);
}

void RwControlLocal::setTransmit(const RwControlTransmit &transmit)
{
	RwControlTransmitMessage *msg = new RwControlTransmitMessage;
	msg->transmit = transmit;
	remote_->postMessage(msg);
}

void RwControlLocal::setRecord(const RwControlRecord &record)
{
	RwControlRecordMessage *msg = new RwControlRecordMessage;
	msg->record = record;
	remote_->postMessage(msg);
}

void RwControlLocal::rtpAudioIn(const PRtpPacket &packet)
{
	remote_->rtpAudioIn(packet);
}

void RwControlLocal::rtpVideoIn(const PRtpPacket &packet)
{
	remote_->rtpVideoIn(packet);
}

// note: this is executed in the remote thread
gboolean RwControlLocal::cb_doCreateRemote(gpointer data)
{
	return ((RwControlLocal *)data)->doCreateRemote();
}

// note: this is executed in the remote thread
gboolean RwControlLocal::doCreateRemote()
{
	QMutexLocker locker(&m);
	timer = 0;
	remote_ = new RwControlRemote(thread_->mainContext(), this);
	w.wakeOne();
	return FALSE;
}

// note: this is executed in the remote thread
gboolean RwControlLocal::cb_doDestroyRemote(gpointer data)
{
	return ((RwControlLocal *)data)->doDestroyRemote();
}

// note: this is executed in the remote thread
gboolean RwControlLocal::doDestroyRemote()
{
	QMutexLocker locker(&m);
	timer = 0;
	delete remote_;
	remote_ = 0;
	w.wakeOne();
	return FALSE;
}

void RwControlLocal::processMessages()
{
	m.lock();
	wake_pending = false;
	QList<RwControlMessage*> list = in;
	in.clear();
	m.unlock();

	QPointer<QObject> self = this;

	// we only care about the latest preview frame
	RwControlFrameMessage *fmsg;
	fmsg = getLatestFrameAndRemoveOthers(&list, RwControlFrame::Preview);
	if(fmsg)
	{
		QImage i = fmsg->frame.image;
		delete fmsg;
		emit previewFrame(i);
		if(!self)
		{
			qDeleteAll(list);
			return;
		}
	}

	// we only care about the latest output frame
	fmsg = getLatestFrameAndRemoveOthers(&list, RwControlFrame::Output);
	if(fmsg)
	{
		QImage i = fmsg->frame.image;
		delete fmsg;
		emit outputFrame(i);
		if(!self)
		{
			qDeleteAll(list);
			return;
		}
	}

	// we only care about the latest audio intensity
	RwControlAudioIntensityMessage *amsg = getLatestAudioIntensityAndRemoveOthers(&list);
	if(amsg)
	{
		int i = amsg->intensity.value;
		delete amsg;
		emit audioIntensityChanged(i);
		if(!self)
		{
			qDeleteAll(list);
			return;
		}
	}

	// process the remaining messages
	while(!list.isEmpty())
	{
		RwControlMessage *msg = list.takeFirst();
		if(msg->type == RwControlMessage::Status)
		{
			RwControlStatusMessage *smsg = (RwControlStatusMessage *)msg;
			RwControlStatus status = smsg->status;
			delete smsg;
			emit statusReady(status);
			if(!self)
			{
				qDeleteAll(list);
				return;
			}
		}
		else
			delete msg;
	}
}

// note: this may be called from the remote thread
void RwControlLocal::postMessage(RwControlMessage *msg)
{
	QMutexLocker locker(&m);

	// if this is a frame, and the queue is maxed, then bump off the
	//   oldest frame to make room
	if(msg->type == RwControlMessage::Frame)
	{
		RwControlFrameMessage *fmsg = (RwControlFrameMessage *)msg;
		int firstPos = -1;
		if(queuedFrameInfo(in, fmsg->frame.type, &firstPos) >= QUEUE_FRAME_MAX)
			in.removeAt(firstPos);
	}

	in += msg;
	if(!wake_pending)
	{
		QMetaObject::invokeMethod(this, "processMessages", Qt::QueuedConnection);
		wake_pending = true;
	}
}

//----------------------------------------------------------------------------
// RwControlRemote
//----------------------------------------------------------------------------
RwControlRemote::RwControlRemote(GMainContext *mainContext, RwControlLocal *local) :
	timer(0),
	blocking(false),
	pending_status(false)
{
	mainContext_ = mainContext;
	local_ = local;
	worker = new RtpWorker(mainContext_);
	worker->app = this;
	worker->cb_started = cb_worker_started;
	worker->cb_updated = cb_worker_updated;
	worker->cb_stopped = cb_worker_stopped;
	worker->cb_finished = cb_worker_finished;
	worker->cb_error = cb_worker_error;
	worker->cb_audioIntensity = cb_worker_audioIntensity;
	worker->cb_previewFrame = cb_worker_previewFrame;
	worker->cb_outputFrame = cb_worker_outputFrame;
	worker->cb_rtpAudioOut = cb_worker_rtpAudioOut;
	worker->cb_rtpVideoOut = cb_worker_rtpVideoOut;
	worker->cb_recordData = cb_worker_recordData;
}

RwControlRemote::~RwControlRemote()
{
	delete worker;

	qDeleteAll(in);
}

gboolean RwControlRemote::cb_processMessages(gpointer data)
{
	return ((RwControlRemote *)data)->processMessages();
}

void RwControlRemote::cb_worker_started(void *app)
{
	((RwControlRemote *)app)->worker_started();
}

void RwControlRemote::cb_worker_updated(void *app)
{
	((RwControlRemote *)app)->worker_updated();
}

void RwControlRemote::cb_worker_stopped(void *app)
{
	((RwControlRemote *)app)->worker_stopped();
}

void RwControlRemote::cb_worker_finished(void *app)
{
	((RwControlRemote *)app)->worker_finished();
}

void RwControlRemote::cb_worker_error(void *app)
{
	((RwControlRemote *)app)->worker_error();
}

void RwControlRemote::cb_worker_audioIntensity(int value, void *app)
{
	((RwControlRemote *)app)->worker_audioIntensity(value);
}

void RwControlRemote::cb_worker_previewFrame(const RtpWorker::Frame &frame, void *app)
{
	((RwControlRemote *)app)->worker_previewFrame(frame);
}

void RwControlRemote::cb_worker_outputFrame(const RtpWorker::Frame &frame, void *app)
{
	((RwControlRemote *)app)->worker_outputFrame(frame);
}

void RwControlRemote::cb_worker_rtpAudioOut(const PRtpPacket &packet, void *app)
{
	((RwControlRemote *)app)->worker_rtpAudioOut(packet);
}

void RwControlRemote::cb_worker_rtpVideoOut(const PRtpPacket &packet, void *app)
{
	((RwControlRemote *)app)->worker_rtpVideoOut(packet);
}

void RwControlRemote::cb_worker_recordData(const QByteArray &packet, void *app)
{
	((RwControlRemote *)app)->worker_recordData(packet);
}

gboolean RwControlRemote::processMessages()
{
	m.lock();
	timer = 0;
	m.unlock();

	while(1)
	{
		m.lock();
		if(in.isEmpty())
		{
			m.unlock();
			break;
		}
		RwControlMessage *msg = in.takeFirst();
		m.unlock();

		bool ret = processMessage(msg);
		delete msg;

		if(!ret)
		{
			m.lock();
			blocking = true;
			if(timer)
			{
				g_source_destroy(timer);
				timer = 0;
			}
			m.unlock();
			break;
		}
	}

	return FALSE;
}

bool RwControlRemote::processMessage(RwControlMessage *msg)
{
	if(msg->type == RwControlMessage::Start)
	{
		RwControlStartMessage *smsg = (RwControlStartMessage *)msg;

		applyDevicesToWorker(worker, smsg->devices);
		applyCodecsToWorker(worker, smsg->codecs);

		pending_status = true;
		worker->start();
		return false;
	}
	else if(msg->type == RwControlMessage::Stop)
	{
		RwControlStopMessage *smsg = (RwControlStopMessage *)msg;
		Q_UNUSED(smsg);

		pending_status = true;
		worker->stop();
		return false;
	}
	else if(msg->type == RwControlMessage::UpdateDevices)
	{
		RwControlUpdateDevicesMessage *umsg = (RwControlUpdateDevicesMessage *)msg;

		applyDevicesToWorker(worker, umsg->devices);

		worker->update();
		return false;
	}
	else if(msg->type == RwControlMessage::UpdateCodecs)
	{
		RwControlUpdateCodecsMessage *umsg = (RwControlUpdateCodecsMessage *)msg;

		applyCodecsToWorker(worker, umsg->codecs);

		pending_status = true;
		worker->update();
		return false;
	}
	else if(msg->type == RwControlMessage::Transmit)
	{
		RwControlTransmitMessage *tmsg = (RwControlTransmitMessage *)msg;

		if(tmsg->transmit.useAudio)
			worker->transmitAudio(tmsg->transmit.audioIndex);
		else
			worker->pauseAudio();

		if(tmsg->transmit.useVideo)
			worker->transmitVideo(tmsg->transmit.videoIndex);
		else
			worker->pauseVideo();
	}
	else if(msg->type == RwControlMessage::Record)
	{
		RwControlRecordMessage *rmsg = (RwControlRecordMessage *)msg;

		if(rmsg->record.enabled)
			worker->recordStart();
		else
			worker->recordStop();
	}

	return true;
}

void RwControlRemote::worker_started()
{
	pending_status = false;
	RwControlStatusMessage *msg = statusFromWorker(worker);
	local_->postMessage(msg);
	resumeMessages();
}

void RwControlRemote::worker_updated()
{
	if(pending_status)
	{
		pending_status = false;
		RwControlStatusMessage *msg = statusFromWorker(worker);
		local_->postMessage(msg);
	}

	resumeMessages();
}

void RwControlRemote::worker_stopped()
{
	pending_status = false;
	RwControlStatusMessage *msg = statusFromWorker(worker);
	msg->status.stopped = true;
	local_->postMessage(msg);
}

void RwControlRemote::worker_finished()
{
	RwControlStatusMessage *msg = statusFromWorker(worker);
	msg->status.finished = true;
	local_->postMessage(msg);
}

void RwControlRemote::worker_error()
{
	RwControlStatusMessage *msg = statusFromWorker(worker);
	msg->status.error = true;
	msg->status.errorCode = worker->error;
	local_->postMessage(msg);
}

void RwControlRemote::worker_audioIntensity(int value)
{
	RwControlAudioIntensityMessage *msg = new RwControlAudioIntensityMessage;
	msg->intensity.value = value;
	local_->postMessage(msg);
}

void RwControlRemote::worker_previewFrame(const RtpWorker::Frame &frame)
{
	RwControlFrameMessage *msg = new RwControlFrameMessage;
	msg->frame.type = RwControlFrame::Preview;
	msg->frame.image = frame.image;
	local_->postMessage(msg);
}

void RwControlRemote::worker_outputFrame(const RtpWorker::Frame &frame)
{
	RwControlFrameMessage *msg = new RwControlFrameMessage;
	msg->frame.type = RwControlFrame::Output;
	msg->frame.image = frame.image;
	local_->postMessage(msg);
}

void RwControlRemote::worker_rtpAudioOut(const PRtpPacket &packet)
{
	if(local_->cb_rtpAudioOut)
		local_->cb_rtpAudioOut(packet, local_->app);
}

void RwControlRemote::worker_rtpVideoOut(const PRtpPacket &packet)
{
	if(local_->cb_rtpVideoOut)
		local_->cb_rtpVideoOut(packet, local_->app);
}

void RwControlRemote::worker_recordData(const QByteArray &packet)
{
	if(local_->cb_recordData)
		local_->cb_recordData(packet, local_->app);
}

void RwControlRemote::resumeMessages()
{
	QMutexLocker locker(&m);
	if(blocking)
	{
		blocking = false;
		if(!in.isEmpty() && !timer)
		{
			timer = g_timeout_source_new(0);
			g_source_set_callback(timer, cb_processMessages, this, NULL);
			g_source_attach(timer, mainContext_);
		}
	}
}

// note: this may be called from the local thread
void RwControlRemote::postMessage(RwControlMessage *msg)
{
	QMutexLocker locker(&m);
	in += msg;
	if(!blocking && !timer)
	{
		timer = g_timeout_source_new(0);
		g_source_set_callback(timer, cb_processMessages, this, NULL);
		g_source_attach(timer, mainContext_);
	}
}

// note: this may be called from the local thread
void RwControlRemote::rtpAudioIn(const PRtpPacket &packet)
{
	worker->rtpAudioIn(packet);
}

// note: this may be called from the local thread
void RwControlRemote::rtpVideoIn(const PRtpPacket &packet)
{
	worker->rtpVideoIn(packet);
}

}
