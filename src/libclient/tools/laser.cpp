// SPDX-License-Identifier: GPL-3.0-or-later
#include "libclient/tools/laser.h"
#include "libclient/net/client.h"
#include "libclient/net/message.h"
#include "libclient/tools/toolcontroller.h"
#include "libclient/utils/cursors.h"

#include <QColor>

namespace tools {

namespace {
constexpr qint64 RainbowUpdateIntervalMs = 200;
constexpr qreal RainbowCycleMs = 10000.0;
}

LaserPointer::LaserPointer(ToolController &owner)
	: Tool(
		  owner, LASERPOINTER, utils::Cursors::arrow(),
		  Capability::AllowToolAdjust1)
{
}

void LaserPointer::begin(const BeginParams &params)
{
	Q_ASSERT(!m_drawing);
	if(params.right) {
		return;
	}

	m_drawing = true;
	m_rainbowTimer.start();
	m_lastRainbowUpdate = 0;

	net::Client *client = m_owner.client();
	uint8_t contextId = client->myId();
	QColor rainbowColor;
	rainbowColor.setHsvF(0.0, 1.0, 1.0);
	uint32_t color = rainbowColor.rgb();
	net::Message messages[] = {
		net::makeLaserTrailMessage(contextId, color, m_persistence),
		net::makeMovePointerMessage(
			contextId, params.point.x() * 4, params.point.y() * 4),
	};
	client->sendMessages(DP_ARRAY_LENGTH(messages), messages);
}

void LaserPointer::motion(const MotionParams &params)
{
	if(!m_drawing) {
		return;
	}

	net::Client *client = m_owner.client();
	const qint64 elapsed = m_rainbowTimer.elapsed();

	// Move messages remain as frequent as normal. Only send a color update
	// every 200 ms so the rainbow does not create unnecessary server traffic.
	if(elapsed - m_lastRainbowUpdate >= RainbowUpdateIntervalMs) {
		const qreal hue = std::fmod(elapsed, RainbowCycleMs) / RainbowCycleMs;
		QColor rainbowColor;
		rainbowColor.setHsvF(hue, 1.0, 1.0);
		client->sendMessage(net::makeLaserTrailMessage(
			client->myId(), rainbowColor.rgb(), m_persistence));
		m_lastRainbowUpdate = elapsed;
	}

	client->sendMessage(net::makeMovePointerMessage(
		client->myId(), params.point.x() * 4, params.point.y() * 4));
}

void LaserPointer::end(const EndParams &)
{
	if(m_drawing) {
		m_drawing = false;
		m_owner.client()->sendMessage(
			 net::makeLaserTrailMessage(m_owner.client()->myId(), 0, 0));
	}
}

}