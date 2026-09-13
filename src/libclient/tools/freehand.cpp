// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include <dpcommon/threading.h>
#include <dpengine/layer_content.h>
#include <dpengine/layer_group.h>
#include <dpmsg/msg_internal.h>
}
#include "libclient/canvas/canvasmodel.h"
#include "libclient/canvas/layerlist.h"
#include "libclient/canvas/paintengine.h"
#include "libclient/net/client.h"
#include "libclient/tools/freehand.h"
#include "libclient/tools/toolcontroller.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QPainterPath>
#include <QtMath>

using std::placeholders::_1;

namespace tools {

AntiOverflowSource::~AntiOverflowSource()
{
	clear();
}

DP_LayerContent *AntiOverflowSource::get(ToolController &owner)
{
	canvas::CanvasModel *canvas = owner.model();
	if(!canvas) {
		clear();
		return nullptr;
	}

	canvas::LayerListModel *layerlist = canvas->layerlist();
	int fillSourceLayerId = layerlist->fillSourceLayerId();
	if(fillSourceLayerId <= 0) {
		clear();
		return nullptr;
	}

	drawdance::LayerSearchResult lsr =
		canvas->paintEngine()->viewCanvasState().searchLayer(
			fillSourceLayerId, true);
	if(drawdance::LayerContent *layerContent =
		   std::get_if<drawdance::LayerContent>(&lsr.data)) {
		return setLayerContentSource(layerContent->get());
	} else if(
		drawdance::LayerGroup *layerGroup =
			std::get_if<drawdance::LayerGroup>(&lsr.data)) {
		return setLayerGroupSource(layerGroup->get(), lsr.props.get());
	} else {
		qWarning("Anti-overflow source %d not found", fillSourceLayerId);
		clear();
		return nullptr;
	}
}

void AntiOverflowSource::clear()
{
	DP_layer_group_decref_nullable(m_sourceLg);
	DP_layer_content_decref_nullable(m_sourceLc);
	m_sourceLg = nullptr;
	m_sourceLc = nullptr;
}

DP_LayerContent *AntiOverflowSource::setLayerContentSource(DP_LayerContent *lc)
{
	Q_ASSERT(lc);
	if(m_sourceLg || m_sourceLc != lc) {
		clear();
		m_sourceLc = DP_layer_content_incref(lc);
	}
	return m_sourceLc;
}

DP_LayerContent *
AntiOverflowSource::setLayerGroupSource(DP_LayerGroup *lg, DP_LayerProps *lp)
{
	Q_ASSERT(lg);
	Q_ASSERT(lp);
	if(m_sourceLg != lg) {
		clear();
		m_sourceLg = DP_layer_group_incref(lg);
		m_sourceLc = DP_transient_layer_content_persist(
			DP_layer_group_merge(lg, lp, false));
	}
	return m_sourceLc;
}


Freehand::Freehand(ToolController &owner, DP_MaskSync *ms)
	: Tool(
		  owner, FREEHAND, Qt::CrossCursor,
		  Capability::AllowColorPick | Capability::SupportsPressure |
			  Capability::AllowToolAdjust1 | Capability::AllowToolAdjust2 |
			  Capability::AllowToolAdjust3)
	, m_strokeWorker(
		  ms, std::bind(&Freehand::pushMessage, this, _1),
		  std::bind(&Freehand::pollControl, this, _1),
		  [this] { return sync(m_sem); })
	, m_verticalStrokeWorker(
		  ms, std::bind(&Freehand::pushMessage, this, _1),
		  std::bind(&Freehand::pollControl, this, _1),
		  [this] { return sync(m_verticalSem); })
	, m_horizontalStrokeWorker(
		  ms, std::bind(&Freehand::pushMessage, this, _1),
		  std::bind(&Freehand::pollControl, this, _1),
		  [this] { return sync(m_horizontalSem); })
	, m_bothStrokeWorker(
		  ms, std::bind(&Freehand::pushMessage, this, _1),
		  std::bind(&Freehand::pollControl, this, _1),
		  [this] { return sync(m_bothSem); })
	, m_mutex(DP_mutex_new())
	, m_sem(DP_semaphore_new(0))
	, m_verticalSem(DP_semaphore_new(0))
	, m_horizontalSem(DP_semaphore_new(0))
	, m_bothSem(DP_semaphore_new(0))
{
	QObject::connect(
		&m_owner, &ToolController::freehandMessagesAvailable, &m_owner,
		std::bind(&Freehand::flushMessages, this), Qt::QueuedConnection);
	m_pollTimer.setSingleShot(false);
	m_pollTimer.setTimerType(Qt::PreciseTimer);
	m_pollTimer.setInterval(15);
	QObject::connect(&m_pollTimer, &QTimer::timeout, [this] {
		poll();
	});
}

Freehand::~Freehand()
{
	DP_semaphore_free(m_bothSem);
	DP_semaphore_free(m_horizontalSem);
	DP_semaphore_free(m_verticalSem);
	DP_semaphore_free(m_sem);
	DP_mutex_free(m_mutex);
}

void Freehand::begin(const BeginParams &params)
{
	beginStroke(params, this);
}

void Freehand::beginStroke(const BeginParams &params, SnapToPixelToggle *target)
{
	if(m_drawing) {
		return;
	}

	QColor color;
	if(params.right) {
		switch(m_owner.freehandRightClickAction()) {
		case int(tools::FreehandRightClickAction::Background):
			color = m_owner.backgroundColor();
			break;
		case int(tools::FreehandRightClickAction::Erase):
			// Already set in ToolController::startDrawing.
			break;
		default:
			return;
		}
	}

	const brushes::ActiveBrush &brush = m_owner.activeBrush();
	bool pixelArtInput = brush.isPixelArtInput();
	target->setSnapToPixel(pixelArtInput);

	const DP_AntiOverflow &antiOverflow = brush.constAntiOverflow();
	DP_LayerContent *floodLc;
	double floodTolerance;
	int floodExpand;
	if(antiOverflow.enabled && !isCompatibilityMode()) {
		floodLc = m_antiOverflowSource.get(m_owner);
		if(!floodLc) {
			emit m_owner.showMessageRequested(
				QCoreApplication::translate(
					"tools::FreehandSettings",
					"Anti-overflow requires a fill source layer."));
			return;
		}
		floodTolerance = double(antiOverflow.tolerance) / 255.0;
		floodExpand = antiOverflow.expand;
	} else {
		floodLc = nullptr;
		floodTolerance = 0.0;
		floodExpand = 0;
	}

	m_drawing = true;
	m_firstPoint = true;

	// If the user starts a new stroke, they presumably don't mean to wait for
	// the previous one, so cancel that. Not doing this will lead to deadlocks
	// in some cases! If the previous stroke was using a thread and the current
	// one doesn't finishing the stroke will call pollControl with a blocking
	// queued connection. So don't change this without considering that.
	m_cancelling = true;
	cancelStroke();
	m_verticalStrokeActive =
		m_symmetryMode == SymmetryMode::Vertical ||
		m_symmetryMode == SymmetryMode::Both;
	m_horizontalStrokeActive =
		m_symmetryMode == SymmetryMode::Horizontal ||
		m_symmetryMode == SymmetryMode::Both;
	m_bothStrokeActive = m_symmetryMode == SymmetryMode::Both;
	if(m_symmetryMode != SymmetryMode::Off) {
		ensureSymmetryCenter();
	}

	m_owner.setStrokeWorkerBrush(
		m_strokeWorker, type(), floodLc, floodTolerance, floodExpand, color);
	if(m_verticalStrokeActive) {
		m_owner.setStrokeWorkerBrush(
			m_verticalStrokeWorker, type(), floodLc, floodTolerance, floodExpand,
			color);
	}
	if(m_horizontalStrokeActive) {
		m_owner.setStrokeWorkerBrush(
			m_horizontalStrokeWorker, type(), floodLc, floodTolerance,
			floodExpand, color);
	}
	if(m_bothStrokeActive) {
		m_owner.setStrokeWorkerBrush(
			m_bothStrokeWorker, type(), floodLc, floodTolerance, floodExpand,
			color);
	}
	m_cancelling = false;

	// The pressure value of the first point is unreliable
	// because it is (or was?) possible to get a synthetic MousePress event
	// before the StylusPress event.
	m_start = params.point;
	m_zoom = params.zoom;
	m_angle = params.angle;
	m_mirror = params.mirror;
	m_flip = params.flip;

	if(pixelArtInput || !m_owner.delayInitialDab()) {
		strokeTo(params.point);
	}
}

void Freehand::motion(const MotionParams &params)
{
	if(m_drawing) {
		strokeTo(params.point);
	}
}

void Freehand::hold(const MotionParams &params)
{
	if(m_drawing && m_firstPoint) {
		m_start.setTimeMsec(params.point.timeMsec());
		m_start.setPressure(params.point.pressure());
		m_start.setXtilt(params.point.xtilt());
		m_start.setYtilt(params.point.ytilt());
		m_start.setRotation(params.point.rotation());
	}
}

void Freehand::hover(const HoverParams &params)
{
	m_lastHoverPoint = params.point;
	m_haveHoverPoint = true;
	updateSymmetryGuide();
}

void Freehand::strokeTo(const canvas::Point &point)
{
	Q_ASSERT(m_drawing);
	drawdance::CanvasState canvasState =
		m_owner.model()->paintEngine()->sampleCanvasState();

	if(m_firstPoint) {
		m_firstPoint = false;
		m_strokeWorker.beginStroke(
			localUserId(), canvasState, isCompatibilityMode(), true, m_mirror,
			m_flip, m_zoom, m_angle);
		m_strokeWorker.strokeTo(m_start, canvasState);

		if(m_verticalStrokeActive) {
			m_verticalStrokeWorker.beginStroke(
				localUserId(), canvasState, isCompatibilityMode(), false,
				m_mirror, m_flip, m_zoom, m_angle);
			m_verticalStrokeWorker.strokeTo(
				symmetryPoint(m_start, true, false), canvasState);
		}
		if(m_horizontalStrokeActive) {
			m_horizontalStrokeWorker.beginStroke(
				localUserId(), canvasState, isCompatibilityMode(), false,
				m_mirror, m_flip, m_zoom, m_angle);
			m_horizontalStrokeWorker.strokeTo(
				symmetryPoint(m_start, false, true), canvasState);
		}
		if(m_bothStrokeActive) {
			m_bothStrokeWorker.beginStroke(
				localUserId(), canvasState, isCompatibilityMode(), false,
				m_mirror, m_flip, m_zoom, m_angle);
			m_bothStrokeWorker.strokeTo(
				symmetryPoint(m_start, true, true), canvasState);
		}
	}

	m_strokeWorker.strokeTo(point, canvasState);
	m_strokeWorker.flushDabs();
	if(m_verticalStrokeActive) {
		m_verticalStrokeWorker.strokeTo(
			symmetryPoint(point, true, false), canvasState);
		m_verticalStrokeWorker.flushDabs();
	}
	if(m_horizontalStrokeActive) {
		m_horizontalStrokeWorker.strokeTo(
			symmetryPoint(point, false, true), canvasState);
		m_horizontalStrokeWorker.flushDabs();
	}
	if(m_bothStrokeActive) {
		m_bothStrokeWorker.strokeTo(
			symmetryPoint(point, true, true), canvasState);
		m_bothStrokeWorker.flushDabs();
	}
}

void Freehand::end(const EndParams &)
{
	if(m_drawing) {
		m_drawing = false;
		drawdance::CanvasState canvasState =
			m_owner.model()->paintEngine()->sampleCanvasState();

		if(m_firstPoint) {
			m_firstPoint = false;
			m_strokeWorker.beginStroke(
				localUserId(), canvasState, isCompatibilityMode(), true,
				m_mirror, m_flip, m_zoom, m_angle);
			m_strokeWorker.strokeTo(m_start, canvasState);

			if(m_verticalStrokeActive) {
				m_verticalStrokeWorker.beginStroke(
					localUserId(), canvasState, isCompatibilityMode(), false,
					m_mirror, m_flip, m_zoom, m_angle);
				m_verticalStrokeWorker.strokeTo(
					symmetryPoint(m_start, true, false), canvasState);
			}
			if(m_horizontalStrokeActive) {
				m_horizontalStrokeWorker.beginStroke(
					localUserId(), canvasState, isCompatibilityMode(), false,
					m_mirror, m_flip, m_zoom, m_angle);
				m_horizontalStrokeWorker.strokeTo(
					symmetryPoint(m_start, false, true), canvasState);
			}
			if(m_bothStrokeActive) {
				m_bothStrokeWorker.beginStroke(
					localUserId(), canvasState, isCompatibilityMode(), false,
					m_mirror, m_flip, m_zoom, m_angle);
				m_bothStrokeWorker.strokeTo(
					symmetryPoint(m_start, true, true), canvasState);
			}
		}

		long long timeMsec = QDateTime::currentMSecsSinceEpoch();
		m_strokeWorker.endStroke(timeMsec, canvasState, true);
		if(m_verticalStrokeActive) {
			m_verticalStrokeWorker.endStroke(timeMsec, canvasState, true);
		}
		if(m_horizontalStrokeActive) {
			m_horizontalStrokeWorker.endStroke(timeMsec, canvasState, true);
		}
		if(m_bothStrokeActive) {
			m_bothStrokeWorker.endStroke(timeMsec, canvasState, true);
		}
	}
}

bool Freehand::undoRedo(bool redo)
{
	cancelStroke();
	m_strokeWorker.pushMessageNoinc(DP_msg_undo_new(localUserId(), 0, redo));
	return true;
}

void Freehand::offsetActiveTool(int x, int y)
{
	m_strokeWorker.addOffset(x, y);
	m_verticalStrokeWorker.addOffset(x, y);
	m_horizontalStrokeWorker.addOffset(x, y);
	m_bothStrokeWorker.addOffset(x, y);
	if(m_symmetryCenterInitialized) {
		m_symmetryCenter += QPointF(x, y);
	}
	if(m_haveHoverPoint) {
		m_lastHoverPoint += QPointF(x, y);
	}
	updateSymmetryGuide();
}

void Freehand::setBrushSizeLimit(int limit)
{
	m_strokeWorker.setSizeLimit(limit);
	m_verticalStrokeWorker.setSizeLimit(limit);
	m_horizontalStrokeWorker.setSizeLimit(limit);
	m_bothStrokeWorker.setSizeLimit(limit);
}

void Freehand::setSelectionMaskingEnabled(bool selectionMaskingEnabled)
{
	setCapability(Capability::IgnoresSelections, !selectionMaskingEnabled);
}

void Freehand::finishWorker(
	drawdance::StrokeWorker &worker, DP_Semaphore *sem, bool wait)
{
	if(worker.isThreadActive()) {
		DP_SEMAPHORE_MUST_POST(sem);
		worker.finishThread();
		if(wait) {
			DP_SEMAPHORE_MUST_WAIT(sem);
		}
	}
}

void Freehand::finish()
{
	m_cancelling = true;
	cancelStroke();
	DP_SEMAPHORE_MUST_POST(m_sem);
	m_strokeWorker.finishThread();
	DP_SEMAPHORE_MUST_WAIT(m_sem);
	finishWorker(m_verticalStrokeWorker, m_verticalSem, true);
	finishWorker(m_horizontalStrokeWorker, m_horizontalSem, true);
	finishWorker(m_bothStrokeWorker, m_bothSem, true);
	m_cancelling = false;
}

void Freehand::dispose()
{
	m_cancelling = true;
	cancelStroke();
	DP_SEMAPHORE_MUST_POST(m_sem);
	m_strokeWorker.finishThread();
	finishWorker(m_verticalStrokeWorker, m_verticalSem, false);
	finishWorker(m_horizontalStrokeWorker, m_horizontalSem, false);
	finishWorker(m_bothStrokeWorker, m_bothSem, false);
}

void Freehand::setSnapToPixel(bool snapToPixel)
{
	setCapability(Capability::SnapsToPixel, snapToPixel);
}

void Freehand::setSymmetryMode(SymmetryMode mode)
{
	if(m_symmetryMode == mode) {
		return;
	}
	m_symmetryMode = mode;
	if(mode != SymmetryMode::Off) {
		ensureSymmetryCenter();
	}
	updateSymmetryGuide();
}

void Freehand::setSymmetryCenterToCursor()
{
	if(m_haveHoverPoint) {
		m_symmetryCenter = m_lastHoverPoint;
		m_symmetryCenterInitialized = true;
	} else {
		ensureSymmetryCenter();
	}
	updateSymmetryGuide();
}

void Freehand::setSymmetryGuidesVisible(bool visible)
{
	if(m_symmetryGuidesVisible != visible) {
		m_symmetryGuidesVisible = visible;
		updateSymmetryGuide();
	}
}

void Freehand::ensureSymmetryCenter()
{
	if(!m_symmetryCenterInitialized) {
		canvas::CanvasModel *model = m_owner.model();
		if(model) {
			QSize size = model->size();
			m_symmetryCenter =
				QPointF(qreal(size.width()) / 2.0, qreal(size.height()) / 2.0);
		} else {
			m_symmetryCenter = m_haveHoverPoint ? m_lastHoverPoint : QPointF();
		}
		m_symmetryCenterInitialized = true;
	}
}

void Freehand::updateSymmetryGuide()
{
	QPainterPath path;
	if(m_symmetryMode != SymmetryMode::Off && m_symmetryGuidesVisible) {
		ensureSymmetryCenter();
		canvas::CanvasModel *model = m_owner.model();
		if(model) {
			QSize size = model->size();
			if(m_symmetryMode == SymmetryMode::Vertical ||
			   m_symmetryMode == SymmetryMode::Both) {
				path.moveTo(m_symmetryCenter.x(), 0.0);
				path.lineTo(m_symmetryCenter.x(), size.height());
			}
			if(m_symmetryMode == SymmetryMode::Horizontal ||
			   m_symmetryMode == SymmetryMode::Both) {
				path.moveTo(0.0, m_symmetryCenter.y());
				path.lineTo(size.width(), m_symmetryCenter.y());
			}
		}
	}
	emit m_owner.pathPreviewRequested(path);
}

canvas::Point Freehand::symmetryPoint(
	const canvas::Point &point, bool vertical, bool horizontal) const
{
	canvas::Point mirrored = point;
	if(vertical) {
		mirrored.setX(2.0 * m_symmetryCenter.x() - mirrored.x());
		mirrored.setXtilt(-mirrored.xtilt());
		mirrored.setRotation(M_PI - mirrored.rotation());
	}
	if(horizontal) {
		mirrored.setY(2.0 * m_symmetryCenter.y() - mirrored.y());
		mirrored.setYtilt(-mirrored.ytilt());
		mirrored.setRotation(-mirrored.rotation());
	}
	return mirrored;
}

void Freehand::cancelStroke()
{
	long long timeMsec = QDateTime::currentMSecsSinceEpoch();
	m_strokeWorker.cancelStroke(timeMsec, true);
	if(m_verticalStrokeActive) {
		m_verticalStrokeWorker.cancelStroke(timeMsec, true);
	}
	if(m_horizontalStrokeActive) {
		m_horizontalStrokeWorker.cancelStroke(timeMsec, true);
	}
	if(m_bothStrokeActive) {
		m_bothStrokeWorker.cancelStroke(timeMsec, true);
	}
	m_verticalStrokeActive = false;
	m_horizontalStrokeActive = false;
	m_bothStrokeActive = false;
}

void Freehand::pushMessage(DP_Message *rawMsg)
{
	net::Message msg = net::Message::noinc(rawMsg);
	net::Client *client = m_owner.client();
	client->sendLocalFreehandMessage(msg);
	if(msg.type() != DP_MSG_INTERNAL) {
		DP_MUTEX_MUST_LOCK(m_mutex);
		bool needsSignal = m_messages.isEmpty();
		m_messages.append(std::move(msg));
		DP_MUTEX_MUST_UNLOCK(m_mutex);
		if(needsSignal) {
			emit m_owner.freehandMessagesAvailable();
		}
	}
}

void Freehand::flushMessages()
{
	DP_MUTEX_MUST_LOCK(m_mutex);
	m_outbox.swap(m_messages);
	DP_MUTEX_MUST_UNLOCK(m_mutex);
	int count = m_outbox.size();
	if(count != 0) {
		m_owner.client()->matchAndSendRemoteMessages(
			count, m_outbox.constData());
		m_outbox.clear();
	}
}

void Freehand::pollControl(bool enable)
{
	auto update = [this, enable] {
		if(enable) {
			++m_pollUsers;
			if(m_pollUsers == 1) {
				m_pollTimer.start();
			}
		} else {
			m_pollUsers = qMax(0, m_pollUsers - 1);
			if(m_pollUsers == 0) {
				m_pollTimer.stop();
			}
		}
	};

	if(isOnMainThread()) {
		update();
	} else {
		QMetaObject::invokeMethod(
			&m_pollTimer, update,
			m_cancelling ? Qt::QueuedConnection : Qt::BlockingQueuedConnection);
	}
}

void Freehand::poll()
{
	drawdance::CanvasState canvasState =
		m_owner.model()->paintEngine()->sampleCanvasState();
	long long timeMsec = QDateTime::currentMSecsSinceEpoch();
	m_strokeWorker.poll(timeMsec, canvasState);
	m_strokeWorker.flushDabs();
	if(m_verticalStrokeActive) {
		m_verticalStrokeWorker.poll(timeMsec, canvasState);
		m_verticalStrokeWorker.flushDabs();
	}
	if(m_horizontalStrokeActive) {
		m_horizontalStrokeWorker.poll(timeMsec, canvasState);
		m_horizontalStrokeWorker.flushDabs();
	}
	if(m_bothStrokeActive) {
		m_bothStrokeWorker.poll(timeMsec, canvasState);
		m_bothStrokeWorker.flushDabs();
	}
}

DP_CanvasState *Freehand::sync(DP_Semaphore *sem)
{
	if(isOnMainThread()) {
		qWarning("Freehand::sync called on main thread");
		return nullptr;
	} else if(m_cancelling) {
		return nullptr;
	} else {
		pushMessage(DP_msg_internal_paint_sync_new(
			0, &Freehand::syncUnlockCallback, sem));
		DP_SEMAPHORE_MUST_WAIT(sem);
		return m_owner.model()->paintEngine()->sampleCanvasState().take();
	}
}

void Freehand::syncUnlockCallback(void *user)
{
	DP_Semaphore *sem = static_cast<DP_Semaphore *>(user);
	DP_SEMAPHORE_MUST_POST(sem);
}

bool Freehand::isOnMainThread()
{
	return QCoreApplication::instance()->thread() == QThread::currentThread();
}


FreehandEraser::FreehandEraser(ToolController &owner, Freehand *freehand)
	: Tool(
		  owner, ERASER, Qt::CrossCursor,
		  Capability::AllowColorPick | Capability::SupportsPressure |
			  Capability::AllowToolAdjust1 | Capability::AllowToolAdjust2 |
			  Capability::AllowToolAdjust3)
	, m_freehand(freehand)
{
}

void FreehandEraser::begin(const BeginParams &params)
{
	m_freehand->beginStroke(params, this);
}

void FreehandEraser::motion(const MotionParams &params)
{
	m_freehand->motion(params);
}

void FreehandEraser::hold(const MotionParams &params)
{
	m_freehand->hold(params);
}

void FreehandEraser::hover(const HoverParams &params)
{
	m_freehand->hover(params);
}

void FreehandEraser::end(const EndParams &params)
{
	m_freehand->end(params);
}

bool FreehandEraser::undoRedo(bool redo)
{
	return m_freehand->undoRedo(redo);
}

void FreehandEraser::offsetActiveTool(int x, int y)
{
	m_freehand->offsetActiveTool(x, y);
}

void FreehandEraser::finish()
{
	m_freehand->finish();
}

void FreehandEraser::setSnapToPixel(bool snapToPixel)
{
	setCapability(Capability::SnapsToPixel, snapToPixel);
}

}
