///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2020 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <QDebug>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QBuffer>

#include "device/deviceapi.h"
#include "dsp/dspcommands.h"
#include "dsp/dspengine.h"
#include "dsp/dspdevicemimoengine.h"
#include "dsp/devicesamplesource.h"
#include "dsp/devicesamplesink.h"
#include "dsp/filerecord.h"
#include "xtrx/devicextrxparam.h"
#include "xtrx/devicextrxshared.h"
#include "xtrx/devicextrx.h"

#include "xtrxmithread.h"
#include "xtrxmothread.h"
#include "xtrxmimo.h"

MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgConfigureXTRXMIMO, Message)
MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgGetStreamInfo, Message)
MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgGetDeviceInfo, Message)
MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgReportClockGenChange, Message)
MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgReportStreamInfo, Message)
MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgFileRecord, Message)
MESSAGE_CLASS_DEFINITION(XTRXMIMO::MsgStartStop, Message)

XTRXMIMO::XTRXMIMO(DeviceAPI *deviceAPI) :
    m_deviceAPI(deviceAPI),
	m_settings(),
    m_sourceThread(nullptr),
    m_sinkThread(nullptr),
	m_deviceDescription("XTRXMIMO"),
	m_runningRx(false),
    m_runningTx(false),
    m_open(false)
{
    m_open = openDevice();
    m_mimoType = MIMOHalfSynchronous;
    m_sampleMIFifo.init(2, 4096 * 64);
    m_sampleMOFifo.init(2, 4096 * 64);
    m_deviceAPI->setNbSourceStreams(2);
    m_deviceAPI->setNbSinkStreams(2);
    m_networkManager = new QNetworkAccessManager();
    connect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
}

XTRXMIMO::~XTRXMIMO()
{
    disconnect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
    delete m_networkManager;
    closeDevice();
}

bool XTRXMIMO::openDevice()
{
    m_deviceShared.m_dev = new DeviceXTRX();
    char serial[256];
    strcpy(serial, qPrintable(m_deviceAPI->getSamplingDeviceSerial()));

    if (!m_deviceShared.m_dev->open(serial))
    {
        qCritical("XTRXMIMO::openDevice: cannot open XTRX device");
        return false;
    }

    return true;
}

void XTRXMIMO::closeDevice()
{
    if (m_runningRx) {
        stopRx();
    }

    if (m_runningTx) {
        stopTx();
    }

    m_deviceShared.m_dev->close();
    delete m_deviceShared.m_dev;
    m_deviceShared.m_dev = nullptr;
}

void XTRXMIMO::destroy()
{
    delete this;
}

void XTRXMIMO::init()
{
    applySettings(m_settings, true);
}

bool XTRXMIMO::startRx()
{
    qDebug("XTRXMIMO::startRx");

    if (!m_open)
    {
        qCritical("XTRXMIMO::startRx: device was not opened");
        return false;
    }

	QMutexLocker mutexLocker(&m_mutex);

    if (m_runningRx) {
        stopRx();
    }

    m_sourceThread = new XTRXMIThread(m_deviceShared.m_dev->getDevice());
    m_sampleMIFifo.reset();
    m_sourceThread->setFifo(&m_sampleMIFifo);
    m_sourceThread->setLog2Decimation(m_settings.m_log2SoftDecim);
	m_sourceThread->startWork();
	mutexLocker.unlock();
	m_runningRx = true;

    return true;
}

bool XTRXMIMO::startTx()
{
    qDebug("XTRXMIMO::startTx");

    if (!m_open)
    {
        qCritical("XTRXMIMO::startTx: device was not opened");
        return false;
    }

	QMutexLocker mutexLocker(&m_mutex);

    if (m_runningRx) {
        stopRx();
    }

    m_sinkThread = new XTRXMOThread(m_deviceShared.m_dev->getDevice());
    m_sampleMOFifo.reset();
    m_sinkThread->setFifo(&m_sampleMOFifo);
    m_sinkThread->setLog2Interpolation(m_settings.m_log2SoftInterp);
	m_sinkThread->startWork();
	mutexLocker.unlock();
	m_runningTx = true;

    return true;
}

void XTRXMIMO::stopRx()
{
    qDebug("XTRXMIMO::stopRx");

    if (!m_sourceThread) {
        return;
    }

	QMutexLocker mutexLocker(&m_mutex);

    m_sourceThread->stopWork();
    delete m_sourceThread;
    m_sourceThread = nullptr;
    m_runningRx = false;
}

void XTRXMIMO::stopTx()
{
    qDebug("XTRXMIMO::stopTx");

    if (!m_sinkThread) {
        return;
    }

	QMutexLocker mutexLocker(&m_mutex);

    m_sinkThread->stopWork();
    delete m_sinkThread;
    m_sinkThread = nullptr;
    m_runningTx = false;
}

QByteArray XTRXMIMO::serialize() const
{
    return m_settings.serialize();
}

bool XTRXMIMO::deserialize(const QByteArray& data)
{
    bool success = true;

    if (!m_settings.deserialize(data))
    {
        m_settings.resetToDefaults();
        success = false;
    }

    MsgConfigureXTRXMIMO* message = MsgConfigureXTRXMIMO::create(m_settings, true);
    m_inputMessageQueue.push(message);

    if (m_guiMessageQueue)
    {
        MsgConfigureXTRXMIMO* messageToGUI = MsgConfigureXTRXMIMO::create(m_settings, true);
        m_guiMessageQueue->push(messageToGUI);
    }

    return success;
}

const QString& XTRXMIMO::getDeviceDescription() const
{
	return m_deviceDescription;
}

int XTRXMIMO::getSourceSampleRate(int index) const
{
    (void) index;
    int rate = m_settings.m_devSampleRate;
    return (rate / (1<<m_settings.m_log2SoftDecim));
}

int XTRXMIMO::getSinkSampleRate(int index) const
{
    (void) index;
    int rate = m_settings.m_devSampleRate;
    return (rate / (1<<m_settings.m_log2SoftInterp));
}

quint64 XTRXMIMO::getSourceCenterFrequency(int index) const
{
    (void) index;
    return m_settings.m_rxCenterFrequency;
}

void XTRXMIMO::setSourceCenterFrequency(qint64 centerFrequency, int index)
{
    (void) index;
    XTRXMIMOSettings settings = m_settings;
    settings.m_rxCenterFrequency = centerFrequency;

    MsgConfigureXTRXMIMO* message = MsgConfigureXTRXMIMO::create(settings, false);
    m_inputMessageQueue.push(message);

    if (m_guiMessageQueue)
    {
        MsgConfigureXTRXMIMO* messageToGUI = MsgConfigureXTRXMIMO::create(settings, false);
        m_guiMessageQueue->push(messageToGUI);
    }
}

quint64 XTRXMIMO::getSinkCenterFrequency(int index) const
{
    (void) index;
    return m_settings.m_txCenterFrequency;
}

void XTRXMIMO::setSinkCenterFrequency(qint64 centerFrequency, int index)
{
    (void) index;
    XTRXMIMOSettings settings = m_settings;
    settings.m_txCenterFrequency = centerFrequency;

    MsgConfigureXTRXMIMO* message = MsgConfigureXTRXMIMO::create(settings, false);
    m_inputMessageQueue.push(message);

    if (m_guiMessageQueue)
    {
        MsgConfigureXTRXMIMO* messageToGUI = MsgConfigureXTRXMIMO::create(settings, false);
        m_guiMessageQueue->push(messageToGUI);
    }
}

uint32_t XTRXMIMO::getDevSampleRate() const
{
    if (m_deviceShared.m_dev) {
        return m_deviceShared.m_dev->getActualInputRate();
    } else {
        return m_settings.m_devSampleRate;
    }
}

uint32_t XTRXMIMO::getLog2HardDecim() const
{
    if (m_deviceShared.m_dev && (m_deviceShared.m_dev->getActualInputRate() != 0.0)) {
        return log2(m_deviceShared.m_dev->getClockGen() / m_deviceShared.m_dev->getActualInputRate() / 4);
    } else {
        return m_settings.m_log2HardDecim;
    }
}

uint32_t XTRXMIMO::getLog2HardInterp() const
{
    if (m_deviceShared.m_dev && (m_deviceShared.m_dev->getActualOutputRate() != 0.0)) {
        return log2(m_deviceShared.m_dev->getClockGen() / m_deviceShared.m_dev->getActualOutputRate() / 4);
    } else {
        return m_settings.m_log2HardInterp;
    }
}

double XTRXMIMO::getClockGen() const
{
    if (m_deviceShared.m_dev) {
        return m_deviceShared.m_dev->getClockGen();
    } else {
        return 0.0;
    }
}

bool XTRXMIMO::handleMessage(const Message& message)
{
    if (MsgConfigureXTRXMIMO::match(message))
    {
        MsgConfigureXTRXMIMO& conf = (MsgConfigureXTRXMIMO&) message;
        qDebug() << "XTRXMIMO::handleMessage: MsgConfigureXTRXMIMO";

        bool success = applySettings(conf.getSettings(), conf.getForce());

        if (!success) {
            qDebug("XTRXMIMO::handleMessage: config error");
        }

        return true;
    }
    else if (MsgFileRecord::match(message))
    {
        // TODO
        // MsgFileRecord& conf = (MsgFileRecord&) message;
        // qDebug() << "XTRXMIMO::handleMessage: MsgFileRecord: " << conf.getStartStop();
        // int istream = conf.getStreamIndex();

        // if (conf.getStartStop())
        // {
        //     if (m_settings.m_fileRecordName.size() != 0) {
        //         m_fileSinks[istream]->setFileName(m_settings.m_fileRecordName + "_0.sdriq");
        //     } else {
        //         m_fileSinks[istream]->genUniqueFileName(m_deviceAPI->getDeviceUID(), istream);
        //     }

        //     m_fileSinks[istream]->startRecording();
        // }
        // else
        // {
        //     m_fileSinks[istream]->stopRecording();
        // }

        return true;
    }
    else if (MsgGetStreamInfo::match(message))
    {
        if (getMessageQueueToGUI() && m_deviceShared.m_dev && m_deviceShared.m_dev->getDevice())
        {
            uint64_t fifolevelRx = 0;
            uint64_t fifolevelTx = 0;

            xtrx_val_get(m_deviceShared.m_dev->getDevice(), XTRX_RX, XTRX_CH_AB, XTRX_PERF_LLFIFO, &fifolevelRx);
            xtrx_val_get(m_deviceShared.m_dev->getDevice(), XTRX_TX, XTRX_CH_AB, XTRX_PERF_LLFIFO, &fifolevelTx);

            MsgReportStreamInfo *report = MsgReportStreamInfo::create(
                        true,
                        true,
                        fifolevelRx,
                        fifolevelTx,
                        65536);

            getMessageQueueToGUI()->push(report);
        }

        return true;
    }
    else if (MsgGetDeviceInfo::match(message))
    {
        double board_temp = 0.0;
        bool gps_locked = false;

        if (!m_deviceShared.m_dev->getDevice() || ((board_temp = m_deviceShared.get_board_temperature() / 256.0) == 0.0)) {
            qDebug("XTRXMIMO::handleMessage: MsgGetDeviceInfo: cannot get board temperature");
        }

        if (!m_deviceShared.m_dev->getDevice()) {
            qDebug("XTRXMIMO::handleMessage: MsgGetDeviceInfo: cannot get GPS lock status");
        } else {
            gps_locked = m_deviceShared.get_gps_status();
        }

        // send to oneself
        if (getMessageQueueToGUI())
        {
            DeviceXTRXShared::MsgReportDeviceInfo *report = DeviceXTRXShared::MsgReportDeviceInfo::create(board_temp, gps_locked);
            getMessageQueueToGUI()->push(report);
        }

        return true;
    }
    else if (MsgStartStop::match(message))
    {
        MsgStartStop& cmd = (MsgStartStop&) message;
        qDebug() << "XTRXMIMO::handleMessage: "
            << " " << (cmd.getRxElseTx() ? "Rx" : "Tx")
            << " MsgStartStop: " << (cmd.getStartStop() ? "start" : "stop");

        bool startStopRxElseTx = cmd.getRxElseTx();

        if (cmd.getStartStop())
        {
            if (m_deviceAPI->initDeviceEngine(startStopRxElseTx ? 0 : 1)) {
                m_deviceAPI->startDeviceEngine(startStopRxElseTx ? 0 : 1);
            }
        }
        else
        {
            m_deviceAPI->stopDeviceEngine(startStopRxElseTx ? 0 : 1);
        }

        if (m_settings.m_useReverseAPI) {
            webapiReverseSendStartStop(cmd.getStartStop());
        }

        return true;
    }
    else
    {
        return false;
    }
}

bool XTRXMIMO::applySettings(const XTRXMIMOSettings& settings, bool force)
{
    QList<QString> reverseAPIKeys;
    bool rxThreadWasRunning = false;
    bool txThreadWasRunning = false;
    bool doRxLPCalibration = false;
    bool doRxChangeSampleRate = false;
    bool doRxChangeFreq = false;
    bool doTxLPCalibration = false;
    bool doTxChangeSampleRate = false;
    bool doTxChangeFreq = false;
    bool forceNCOFrequencyRx = false;
    bool forceNCOFrequencyTx = false;
    bool forwardChangeRxDSP = false;
    bool forwardChangeTxDSP = false;

    qint64 rxXlatedDeviceCenterFrequency = settings.m_rxCenterFrequency;
    qint64 txXlatedDeviceCenterFrequency = settings.m_txCenterFrequency;

    qDebug() << "XTRXMIMO::applySettings: common:"
        << " m_devSampleRate: " << settings.m_devSampleRate
        << " m_extClock: " << settings.m_extClock
        << " m_extClockFreq: " << settings.m_extClockFreq
        << " force: " << force;
    qDebug() << "XTRXMIMO::applySettings: Rx:"
        << " m_rxCenterFrequency: " << settings.m_txCenterFrequency
        << " m_log2HardDecim: " << settings.m_log2HardDecim
        << " m_log2SoftDecim: " << settings.m_log2SoftDecim
        << " m_dcBlock: " << settings.m_dcBlock
        << " m_iqCorrection: " << settings.m_iqCorrection
        << " m_ncoEnableRx: " << settings.m_ncoEnableRx
        << " m_ncoFrequencyRx: " << settings.m_ncoFrequencyRx
        << " m_antennaPathRx: " << settings.m_antennaPathRx;
    qDebug() << "XTRXMIMO::applySettings: Rx0:"
        << " m_gainRx0: " << settings.m_gainRx0
        << " m_lpfBWRx0: " << settings.m_lpfBWRx0
        << " m_pwrmodeRx0: " << settings.m_pwrmodeRx0;
    qDebug() << "XTRXMIMO::applySettings: Rx1:"
        << " m_gainRx1: " << settings.m_gainRx1
        << " m_lpfBWRx1: " << settings.m_lpfBWRx1
        << " m_pwrmodeRx1: " << settings.m_pwrmodeRx1;
    qDebug() << "XTRXMIMO::applySettings: Tx:"
        << " m_txCenterFrequency: " << settings.m_txCenterFrequency
        << " m_log2HardInterp: " << settings.m_log2HardInterp
        << " m_log2SoftInterp: " << settings.m_log2SoftInterp
        << " m_ncoEnableTx0: " << settings.m_ncoEnableTx
        << " m_ncoFrequencyTx: " << settings.m_ncoFrequencyTx
        << " m_antennaPathTx: " << settings.m_antennaPathTx;
    qDebug() << "XTRXMIMO::applySettings: Tx0:"
        << " m_gainTx0: " << settings.m_gainTx0
        << " m_lpfBWTx0: " << settings.m_lpfBWTx0
        << " m_pwrmodeTx0: " << settings.m_pwrmodeTx0;
    qDebug() << "XTRXMIMO::applySettings: Tx0:"
        << " m_gainTx1: " << settings.m_gainTx1
        << " m_lpfBWTx1: " << settings.m_lpfBWTx1
        << " m_pwrmodeTx1: " << settings.m_pwrmodeTx1;

    // common

    if ((m_settings.m_extClock != settings.m_extClock) || force) {
        reverseAPIKeys.append("extClock");
    }
    if ((m_settings.m_extClockFreq != settings.m_extClockFreq) || force) {
        reverseAPIKeys.append("extClockFreq");
    }

    if ((m_settings.m_extClock != settings.m_extClock)
     || (settings.m_extClock && (m_settings.m_extClockFreq != settings.m_extClockFreq)) || force)
    {
        if (m_deviceShared.m_dev->getDevice() != 0)
        {
            xtrx_set_ref_clk(m_deviceShared.m_dev->getDevice(),
                             (settings.m_extClock) ? settings.m_extClockFreq : 0,
                             (settings.m_extClock) ? XTRX_CLKSRC_EXT : XTRX_CLKSRC_INT);
            {
                doRxChangeSampleRate = true;
                doTxChangeSampleRate = true;
                doRxChangeFreq = true;
                doTxChangeFreq = true;
                qDebug("XTRXMIMO::applySettings: clock set to %s (Ext: %d Hz)",
                       settings.m_extClock ? "external" : "internal",
                       settings.m_extClockFreq);
            }
        }
    }

    if ((m_settings.m_devSampleRate != settings.m_devSampleRate) || force) {
        reverseAPIKeys.append("devSampleRate");
    }

    // Rx

    if ((m_settings.m_dcBlock != settings.m_dcBlock) || force)
    {
        reverseAPIKeys.append("dcBlock");
        m_deviceAPI->configureCorrections(settings.m_dcBlock, settings.m_iqCorrection);
    }

    if ((m_settings.m_iqCorrection != settings.m_iqCorrection) || force)
    {
        reverseAPIKeys.append("iqCorrection");
        m_deviceAPI->configureCorrections(settings.m_dcBlock, settings.m_iqCorrection);
    }

    if ((m_settings.m_log2HardDecim != settings.m_log2HardDecim) || force) {
        reverseAPIKeys.append("log2HardDecim");
    }

    if ((m_settings.m_devSampleRate != settings.m_devSampleRate)
     || (m_settings.m_log2HardDecim != settings.m_log2HardDecim) || force)
    {
        forwardChangeRxDSP = true;

        if (m_deviceShared.m_dev->getDevice()) {
            doTxChangeSampleRate = true;
        }
    }

    if ((m_settings.m_log2SoftDecim != settings.m_log2SoftDecim) || force)
    {
        reverseAPIKeys.append("log2SoftDecim");
        forwardChangeRxDSP = true;

        if (m_sourceThread)
        {
            m_sourceThread->setLog2Decimation(settings.m_log2SoftDecim);
            qDebug() << "XTRXMIMO::applySettings: set soft decimation to " << (1<<settings.m_log2SoftDecim);
        }
    }

    if ((m_settings.m_ncoFrequencyRx != settings.m_ncoFrequencyRx) || force) {
        reverseAPIKeys.append("ncoFrequencyRx");
    }
    if ((m_settings.m_ncoEnableRx != settings.m_ncoEnableRx) || force) {
        reverseAPIKeys.append("ncoEnableRx");
    }

    if ((m_settings.m_ncoFrequencyRx != settings.m_ncoFrequencyRx)
     || (m_settings.m_ncoEnableRx != settings.m_ncoEnableRx) || force)
    {
        forceNCOFrequencyRx = true;
    }

    if ((m_settings.m_antennaPathRx != settings.m_antennaPathRx) || force)
    {
        reverseAPIKeys.append("antennaPathRx");

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_set_antenna(m_deviceShared.m_dev->getDevice(), toXTRXAntennaRx(settings.m_antennaPathRx)) < 0) {
                qCritical("XTRXMIMO::applySettings: could not set antenna path of Rx to %d", (int) settings.m_antennaPathRx);
            } else {
                qDebug("XTRXMIMO::applySettings: set Rx antenna path to %d", (int) settings.m_antennaPathRx);
            }
        }
    }

    if ((m_settings.m_rxCenterFrequency != settings.m_rxCenterFrequency) || force)
    {
        reverseAPIKeys.append("rxCenterFrequency");
        doRxChangeFreq = true;
    }

    // Rx0/1

    if ((m_settings.m_pwrmodeRx0 != settings.m_pwrmodeRx0))
    {
        reverseAPIKeys.append("pwrmodeRx0");

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_val_set(m_deviceShared.m_dev->getDevice(),
                    XTRX_TRX,
                    XTRX_CH_A,
                    XTRX_LMS7_PWR_MODE,
                    settings.m_pwrmodeRx0) < 0) {
                qCritical("XTRXMIMO::applySettings: could not set Rx0 power mode %d", settings.m_pwrmodeRx0);
            }
        }
    }

    if ((m_settings.m_pwrmodeRx1 != settings.m_pwrmodeRx1))
    {
        reverseAPIKeys.append("pwrmodeRx1");

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_val_set(m_deviceShared.m_dev->getDevice(),
                    XTRX_TRX,
                    XTRX_CH_B,
                    XTRX_LMS7_PWR_MODE,
                    settings.m_pwrmodeRx1) < 0) {
                qCritical("XTRXMIMO::applySettings: could not set Rx1 power mode %d", settings.m_pwrmodeRx0);
            }
        }
    }

    if ((m_settings.m_gainModeRx0 != settings.m_gainModeRx0) || force) {
        reverseAPIKeys.append("gainModeRx0");
    }
    if ((m_settings.m_gainRx0 != settings.m_gainRx0) || force) {
        reverseAPIKeys.append("gainRx0");
    }
    if ((m_settings.m_lnaGainRx0 != settings.m_lnaGainRx0) || force) {
        reverseAPIKeys.append("lnaGainRx0");
    }
    if ((m_settings.m_tiaGainRx0 != settings.m_tiaGainRx0) || force) {
        reverseAPIKeys.append("tiaGainRx0");
    }
    if ((m_settings.m_pgaGainRx0 != settings.m_pgaGainRx0) || force) {
        reverseAPIKeys.append("pgaGainRx0");
    }

    if ((m_settings.m_gainModeRx1 != settings.m_gainModeRx1) || force) {
        reverseAPIKeys.append("gainModeRx1");
    }
    if ((m_settings.m_gainRx1 != settings.m_gainRx1) || force) {
        reverseAPIKeys.append("gainRx1");
    }
    if ((m_settings.m_lnaGainRx1 != settings.m_lnaGainRx1) || force) {
        reverseAPIKeys.append("lnaGainRx1");
    }
    if ((m_settings.m_tiaGainRx1 != settings.m_tiaGainRx1) || force) {
        reverseAPIKeys.append("tiaGainRx1");
    }
    if ((m_settings.m_pgaGainRx1 != settings.m_pgaGainRx1) || force) {
        reverseAPIKeys.append("pgaGainRx1");
    }

    if (m_deviceShared.m_dev->getDevice())
    {
        bool doGainAuto = false;
        bool doGainLna = false;
        bool doGainTia = false;
        bool doGainPga = false;

        if ((m_settings.m_gainModeRx0 != settings.m_gainModeRx0) || force)
        {
            if (settings.m_gainModeRx0 == XTRXMIMOSettings::GAIN_AUTO)
            {
                doGainAuto = true;
            }
            else
            {
                doGainLna = true;
                doGainTia = true;
                doGainPga = true;
            }
        }
        else if (m_settings.m_gainModeRx0 == XTRXMIMOSettings::GAIN_AUTO)
        {
            if (m_settings.m_gainRx0 != settings.m_gainRx0) {
                doGainAuto = true;
            }
        }
        else if (m_settings.m_gainModeRx0 == XTRXMIMOSettings::GAIN_MANUAL)
        {
            if (m_settings.m_lnaGainRx0 != settings.m_lnaGainRx0) {
                doGainLna = true;
            }
            if (m_settings.m_tiaGainRx0 != settings.m_tiaGainRx0) {
                doGainTia = true;
            }
            if (m_settings.m_pgaGainRx0 != settings.m_pgaGainRx0) {
                doGainPga = true;
            }
        }

        if (doGainAuto) {
            applyGainAuto(0, m_settings.m_gainRx0);
        }
        if (doGainLna) {
            applyGainLNA(0, m_settings.m_lnaGainRx0);
        }
        if (doGainTia) {
            applyGainTIA(0, tiaToDB(m_settings.m_tiaGainRx0));
        }
        if (doGainPga) {
            applyGainPGA(0, m_settings.m_pgaGainRx0);
        }

        doGainAuto = false;
        doGainLna = false;
        doGainTia = false;
        doGainPga = false;

        if ((m_settings.m_gainModeRx1 != settings.m_gainModeRx1) || force)
        {
            if (settings.m_gainModeRx1 == XTRXMIMOSettings::GAIN_AUTO)
            {
                doGainAuto = true;
            }
            else
            {
                doGainLna = true;
                doGainTia = true;
                doGainPga = true;
            }
        }
        else if (m_settings.m_gainModeRx1 == XTRXMIMOSettings::GAIN_AUTO)
        {
            if (m_settings.m_gainRx1 != settings.m_gainRx1) {
                doGainAuto = true;
            }
        }
        else if (m_settings.m_gainModeRx1 == XTRXMIMOSettings::GAIN_MANUAL)
        {
            if (m_settings.m_lnaGainRx1 != settings.m_lnaGainRx1) {
                doGainLna = true;
            }
            if (m_settings.m_tiaGainRx1 != settings.m_tiaGainRx1) {
                doGainTia = true;
            }
            if (m_settings.m_pgaGainRx1 != settings.m_pgaGainRx1) {
                doGainPga = true;
            }
        }

        if (doGainAuto) {
            applyGainAuto(1, m_settings.m_gainRx1);
        }
        if (doGainLna) {
            applyGainLNA(1, m_settings.m_lnaGainRx1);
        }
        if (doGainTia) {
            applyGainTIA(1, tiaToDB(m_settings.m_tiaGainRx1));
        }
        if (doGainPga) {
            applyGainPGA(1, m_settings.m_pgaGainRx1);
        }
    }

    if ((m_settings.m_lpfBWRx0 != settings.m_lpfBWRx0) || force)
    {
        reverseAPIKeys.append("lpfBWRx0");

        if (m_deviceShared.m_dev->getDevice()) {
            doRxLPCalibration = true;
        }
    }

    if ((m_settings.m_lpfBWRx1 != settings.m_lpfBWRx1) || force)
    {
        reverseAPIKeys.append("lpfBWRx1");

        if (m_deviceShared.m_dev->getDevice()) {
            doRxLPCalibration = true;
        }
    }

    // Tx

    if ((m_settings.m_log2HardInterp != settings.m_log2HardInterp) || force) {
        reverseAPIKeys.append("log2HardInterp");
    }

    if ((m_settings.m_devSampleRate != settings.m_devSampleRate)
     || (m_settings.m_log2HardInterp != settings.m_log2HardInterp) || force)
    {
        forwardChangeTxDSP = true;

        if (m_deviceShared.m_dev->getDevice()) {
            doTxChangeSampleRate = true;
        }
    }

    if ((m_settings.m_log2SoftInterp != settings.m_log2SoftInterp) || force)
    {
        reverseAPIKeys.append("log2SoftInterp");
        forwardChangeTxDSP = true;

        if (m_sinkThread)
        {
            m_sinkThread->setLog2Interpolation(settings.m_log2SoftInterp);
            qDebug("XTRXMIMO::applySettings: set soft interpolation to %u", (1<<settings.m_log2SoftInterp));
        }
    }

    if ((m_settings.m_devSampleRate != settings.m_devSampleRate)
     || (m_settings.m_log2SoftInterp != settings.m_log2SoftInterp) || force)
    {
        unsigned int fifoRate = std::max(
            (unsigned int) settings.m_devSampleRate / (1<<settings.m_log2SoftInterp),
            DeviceXTRXShared::m_sampleFifoMinRate);
        m_sampleMOFifo.resize(SampleMOFifo::getSizePolicy(fifoRate));
    }

    if ((m_settings.m_ncoFrequencyTx != settings.m_ncoFrequencyTx) || force) {
        reverseAPIKeys.append("ncoFrequencyTx");
    }
    if ((m_settings.m_ncoEnableTx != settings.m_ncoEnableTx) || force) {
        reverseAPIKeys.append("ncoEnableTx");
    }

    if ((m_settings.m_ncoFrequencyTx != settings.m_ncoFrequencyTx)
     || (m_settings.m_ncoEnableTx != settings.m_ncoEnableTx) || force)
    {
        forceNCOFrequencyTx = true;
    }

    if ((m_settings.m_antennaPathTx != settings.m_antennaPathTx) || force)
    {
        reverseAPIKeys.append("antennaPathTx");

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_set_antenna(m_deviceShared.m_dev->getDevice(), toXTRXAntennaTx(settings.m_antennaPathTx)) < 0) {
                qCritical("XTRXMIMO::applySettings: could not set Tx antenna path to %d", (int) settings.m_antennaPathTx);
            } else {
                qDebug("XTRXMIMO::applySettings: set Tx antenna path to %d", (int) settings.m_antennaPathTx);
            }
        }
    }

    if ((m_settings.m_txCenterFrequency != settings.m_txCenterFrequency) || force)
    {
        reverseAPIKeys.append("txCenterFrequency");
        doTxChangeFreq = true;
    }

    // Tx0

    if ((m_settings.m_pwrmodeTx0 != settings.m_pwrmodeTx0))
    {
        reverseAPIKeys.append("pwrmodeTx0");

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_val_set(m_deviceShared.m_dev->getDevice(),
                    XTRX_TRX,
                    XTRX_CH_A,
                    XTRX_LMS7_PWR_MODE,
                    settings.m_pwrmodeTx0) < 0) {
                qCritical("XTRXMIMO::applySettings: could not set Tx0 power mode %d", settings.m_pwrmodeTx0);
            }
        }
    }

    if ((m_settings.m_gainTx0 != settings.m_gainTx0) || force)
    {
        reverseAPIKeys.append("gainTx0");

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_set_gain(m_deviceShared.m_dev->getDevice(),
                    XTRX_CH_A,
                    XTRX_TX_PAD_GAIN,
                    settings.m_gainTx0,
                    0) < 0) {
                qDebug("XTRXMIMO::applySettings: Tx0 gain (PAD) set to %u failed", settings.m_gainTx0);
            } else {
                qDebug("XTRXMIMO::applySettings: Tx0 gain (PAD) set to %u", settings.m_gainTx0);
            }
        }
    }

    if ((m_settings.m_lpfBWTx0 != settings.m_lpfBWTx0) || force)
    {
        reverseAPIKeys.append("lpfBWTx0");

        if (m_deviceShared.m_dev->getDevice()) {
            doTxLPCalibration = true;
        }
    }

    // Reverse API

    if (settings.m_useReverseAPI)
    {
        bool fullUpdate = ((m_settings.m_useReverseAPI != settings.m_useReverseAPI) && settings.m_useReverseAPI) ||
                (m_settings.m_reverseAPIAddress != settings.m_reverseAPIAddress) ||
                (m_settings.m_reverseAPIPort != settings.m_reverseAPIPort) ||
                (m_settings.m_reverseAPIDeviceIndex != settings.m_reverseAPIDeviceIndex);
        webapiReverseSendSettings(reverseAPIKeys, settings, fullUpdate || force);
    }

    // Post Rx

    if (doRxChangeSampleRate && (settings.m_devSampleRate != 0))
    {
        // if (m_sourceThread && m_sourceThread->isRunning())
        // {
        //     m_sourceThread->stopWork();
        //     rxThreadWasRunning = true;
        // }

        double master = (settings.m_log2HardDecim == 0) ? 0 : (settings.m_devSampleRate * 4 * (1 << settings.m_log2HardDecim));

        if (m_deviceShared.m_dev->setSamplerate(settings.m_devSampleRate,
                master, //(settings.m_devSampleRate<<settings.m_log2HardDecim)*4,
                false) < 0)
        {
            qCritical("XTRXMIMO::applySettings: could not set sample rate to %f with oversampling of %d",
                      settings.m_devSampleRate,
                      1<<settings.m_log2HardDecim);
        }
        else
        {
            doRxChangeFreq = true;
            forceNCOFrequencyRx = true;
            forwardChangeRxDSP = true;

            qDebug("XTRXMIMO::applySettings: sample rate set to %f with oversampling of %d",
                   m_deviceShared.m_dev->getActualInputRate(),
                   1 << getLog2HardDecim());
        }

        // if (rxThreadWasRunning) {
        //     m_sourceThread->startWork();
        // }
    }

    if (doRxLPCalibration)
    {
        if (xtrx_tune_rx_bandwidth(m_deviceShared.m_dev->getDevice(),
                XTRX_CH_A,
                settings.m_lpfBWRx0,
                0) < 0) {
            qCritical("XTRXMIMO::applySettings: could not set Rx0 LPF to %f Hz", settings.m_lpfBWRx0);
        } else {
            qDebug("XTRXMIMO::applySettings: Rx0 LPF set to %f Hz", settings.m_lpfBWRx0);
        }

        if (xtrx_tune_rx_bandwidth(m_deviceShared.m_dev->getDevice(),
                XTRX_CH_B,
                settings.m_lpfBWRx1,
                0) < 0) {
            qCritical("XTRXMIMO::applySettings: could not set Rx1 LPF to %f Hz", settings.m_lpfBWRx1);
        } else {
            qDebug("XTRXMIMO::applySettings: Rx1 LPF set to %f Hz", settings.m_lpfBWRx1);
        }
    }

    if (doRxChangeFreq)
    {
        forwardChangeRxDSP = true;

        if (m_deviceShared.m_dev->getDevice())
        {
            qint64 deviceCenterFrequency = DeviceSampleSource::calculateDeviceCenterFrequency(
                    rxXlatedDeviceCenterFrequency,
                    0,
                    settings.m_log2SoftDecim,
                    DeviceSampleSource::FC_POS_CENTER,
                    settings.m_devSampleRate,
                    DeviceSampleSource::FrequencyShiftScheme::FSHIFT_STD,
                    false);
            setRxDeviceCenterFrequency(m_deviceShared.m_dev->getDevice(), deviceCenterFrequency, 0);
        }
    }

    if (forceNCOFrequencyRx)
    {
        forwardChangeRxDSP = true;

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_tune_ex(m_deviceShared.m_dev->getDevice(),
                    XTRX_TUNE_BB_RX,
                    XTRX_CH_AB,
                    (settings.m_ncoEnableRx) ? settings.m_ncoFrequencyRx : 0,
                    nullptr) < 0)
            {
                qCritical("XTRXMIMO::applySettings: could not %s and set Rx NCO to %d Hz",
                          settings.m_ncoEnableRx ? "enable" : "disable",
                          settings.m_ncoFrequencyRx);
            }
            else
            {
                qDebug("XTRXMIMO::applySettings: %sd and set NCO Rx to %d Hz",
                       settings.m_ncoEnableRx ? "enable" : "disable",
                       settings.m_ncoFrequencyRx);
            }
        }
    }

    // Post Tx

    if (doTxChangeSampleRate && (settings.m_devSampleRate != 0))
    {
        // if (m_sinkThread && m_sinkThread->isRunning())
        // {
        //     m_sinkThread->stopWork();
        //     txThreadWasRunning = true;
        // }

        double master = (settings.m_log2HardInterp == 0) ? 0 : (settings.m_devSampleRate * 4 * (1 << settings.m_log2HardInterp));

        if (m_deviceShared.m_dev->setSamplerate(settings.m_devSampleRate,
                master, //(settings.m_devSampleRate<<settings.m_log2HardDecim)*4,
                true) < 0)
        {
            qCritical("XTRXMIMO::applySettings: could not set sample rate to %f with oversampling of %d",
                      settings.m_devSampleRate,
                      1<<settings.m_log2HardInterp);
        }
        else
        {
            doTxChangeFreq = true;
            forceNCOFrequencyTx = true;
            forwardChangeTxDSP = true;

            qDebug("XTRXMIMO::applySettings: sample rate set to %f with oversampling of %d",
                   m_deviceShared.m_dev->getActualOutputRate(),
                   1 << getLog2HardInterp());
        }

        // if (txThreadWasRunning) {
        //     m_sinkThread->startWork();
        // }
    }

    if (doTxLPCalibration)
    {
        if (xtrx_tune_tx_bandwidth(m_deviceShared.m_dev->getDevice(),
                XTRX_CH_A,
                settings.m_lpfBWTx0,
                0) < 0) {
            qCritical("XTRXMIMO::applySettings: could not set Tx0 LPF to %f Hz", settings.m_lpfBWTx0);
        } else {
            qDebug("XTRXMIMO::applySettings: Tx0 LPF set to %f Hz", settings.m_lpfBWTx0);
        }

        if (xtrx_tune_tx_bandwidth(m_deviceShared.m_dev->getDevice(),
                XTRX_CH_B,
                settings.m_lpfBWTx1,
                0) < 0) {
            qCritical("XTRXMIMO::applySettings: could not set Tx1 LPF to %f Hz", settings.m_lpfBWTx1);
        } else {
            qDebug("XTRXMIMO::applySettings: Tx1 LPF set to %f Hz", settings.m_lpfBWTx1);
        }
    }

    if (doTxChangeFreq)
    {
        forwardChangeTxDSP = true;

        if (m_deviceShared.m_dev->getDevice())
        {
            qint64 deviceCenterFrequency = DeviceSampleSink::calculateDeviceCenterFrequency(
                settings.m_txCenterFrequency,
                0,
                settings.m_log2SoftInterp,
                DeviceSampleSink::FC_POS_CENTER,
                settings.m_devSampleRate,
                false);
            setTxDeviceCenterFrequency(m_deviceShared.m_dev->getDevice(), deviceCenterFrequency, 0);
        }
    }

    if (forceNCOFrequencyTx)
    {
        forwardChangeTxDSP = true;

        if (m_deviceShared.m_dev->getDevice())
        {
            if (xtrx_tune_ex(m_deviceShared.m_dev->getDevice(),
                    XTRX_TUNE_BB_TX,
                    XTRX_CH_AB,
                    (settings.m_ncoEnableTx) ? settings.m_ncoFrequencyTx : 0,
                    nullptr) < 0)
            {
                qCritical("XTRXMIMO::applySettings: could not %s and set Tx NCO to %d Hz",
                          settings.m_ncoEnableTx ? "enable" : "disable",
                          settings.m_ncoFrequencyTx);
            }
            else
            {
                qDebug("XTRXMIMO::applySettings: %sd and set Tx NCO to %d Hz",
                       settings.m_ncoEnableTx ? "enable" : "disable",
                       settings.m_ncoFrequencyTx);
            }
        }
    }

    // forward changes

    if (forwardChangeRxDSP || forwardChangeTxDSP)
    {
        if (getMessageQueueToGUI())
        {
            MsgReportClockGenChange *report = MsgReportClockGenChange::create();
            getMessageQueueToGUI()->push(report);
        }
    }

    if (forwardChangeRxDSP)
    {
        int sampleRate = settings.m_devSampleRate/(1<<settings.m_log2SoftDecim);
        int ncoShift = settings.m_ncoEnableRx ? settings.m_ncoFrequencyRx : 0;
        DSPMIMOSignalNotification *notif0 = new DSPMIMOSignalNotification(sampleRate, settings.m_rxCenterFrequency + ncoShift, true, 0);
        m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif0);
        DSPMIMOSignalNotification *notif1 = new DSPMIMOSignalNotification(sampleRate, settings.m_rxCenterFrequency + ncoShift, true, 1);
        m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif1);
    }

    if (forwardChangeTxDSP)
    {
        int sampleRate = settings.m_devSampleRate/(1<<settings.m_log2SoftInterp);
        int ncoShift = settings.m_ncoEnableTx ? settings.m_ncoFrequencyTx : 0;
        DSPMIMOSignalNotification *notif0 = new DSPMIMOSignalNotification(sampleRate, settings.m_txCenterFrequency + ncoShift, false, 0);
        m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif0);
        DSPMIMOSignalNotification *notif1 = new DSPMIMOSignalNotification(sampleRate, settings.m_txCenterFrequency + ncoShift, false, 1);
        m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif1);
    }

    m_settings = settings;
    return true;
}

void XTRXMIMO::applyGainAuto(unsigned int channel, uint32_t gain)
{
    uint32_t lna, tia, pga;

    DeviceXTRX::getAutoGains(gain, lna, tia, pga);

    applyGainLNA(channel, lna);
    applyGainTIA(channel, tiaToDB(tia));
    applyGainPGA(channel, pga);
}

void XTRXMIMO::applyGainLNA(unsigned int channel, double gain)
{
    if (xtrx_set_gain(m_deviceShared.m_dev->getDevice(),
            channel == 0 ? XTRX_CH_A : XTRX_CH_B,
            XTRX_RX_LNA_GAIN,
            gain,
            0) < 0) {
        qDebug("XTRXMIMO::applyGainLNA: set Rx%u gain (LNA) to %f failed", channel, gain);
    } else {
        qDebug("XTRXMIMO::applyGainLNA: Rx%u gain (LNA) set to %f", channel, gain);
    }
}

void XTRXMIMO::applyGainTIA(unsigned int channel, double gain)
{
    if (xtrx_set_gain(m_deviceShared.m_dev->getDevice(),
            channel == 0 ? XTRX_CH_A : XTRX_CH_B,
            XTRX_RX_TIA_GAIN,
            gain,
            0) < 0) {
        qDebug("XTRXMIMO::applyGainTIA: set Rx%u gain (TIA) to %f failed", channel, gain);
    } else {
        qDebug("XTRXMIMO::applyGainTIA: Rx%u gain (TIA) set to %f", channel, gain);
    }
}

void XTRXMIMO::applyGainPGA(unsigned int channel, double gain)
{
    if (xtrx_set_gain(m_deviceShared.m_dev->getDevice(),
            channel == 0 ? XTRX_CH_A : XTRX_CH_B,
            XTRX_RX_PGA_GAIN,
            gain,
            0) < 0)
    {
        qDebug("XTRXMIMO::applyGainPGA: set Rx%u gain (PGA) to %f failed", channel, gain);
    }
    else
    {
        qDebug("XTRXMIMO::applyGainPGA: Rx%u gain (PGA) set to %f", channel, gain);
    }
}

double XTRXMIMO::tiaToDB(unsigned idx)
{
    switch (idx) {
    case 1: return 12;
    case 2: return 9;
    default: return 0;
    }
}

xtrx_antenna_t XTRXMIMO::toXTRXAntennaRx(XTRXMIMOSettings::RxAntenna antennaPath)
{
    switch (antennaPath) {
        case XTRXMIMOSettings::RXANT_LO: return XTRX_RX_L;
        case XTRXMIMOSettings::RXANT_HI: return XTRX_RX_H;
        default: return XTRX_RX_W;
    }
}

xtrx_antenna_t XTRXMIMO::toXTRXAntennaTx(XTRXMIMOSettings::TxAntenna antennaPath)
{
    switch (antennaPath) {
        case XTRXMIMOSettings::TXANT_HI: return XTRX_TX_H;
        default: return XTRX_TX_W;
    }
}

void XTRXMIMO::getLORange(float& minF, float& maxF, float& stepF) const
{
    minF = 29e6;
    maxF = 3840e6;
    stepF = 10;
    qDebug("XTRXMIMO::getLORange: min: %f max: %f step: %f",
           minF, maxF, stepF);
}

void XTRXMIMO::getSRRange(float& minF, float& maxF, float& stepF) const
{
    minF = 100e3;
    maxF = 120e6;
    stepF = 10;
    qDebug("XTRXMIMO::getSRRange: min: %f max: %f step: %f",
           minF, maxF, stepF);
}

void XTRXMIMO::getLPRange(float& minF, float& maxF, float& stepF) const
{
    minF = 500e3;
    maxF = 130e6;
    stepF = 10;
    qDebug("XTRXMIMO::getLPRange: min: %f max: %f step: %f",
           minF, maxF, stepF);
}

void XTRXMIMO::setRxDeviceCenterFrequency(xtrx_dev *dev, quint64 freq_hz, int loPpmTenths)
{
    if (dev)
    {
        if (xtrx_tune(dev,
                XTRX_TUNE_RX_FDD,
                freq_hz,
                0) < 0) {
            qCritical("XTRXMIMO::setRxDeviceCenterFrequency: could not set Rx frequency to %llu", freq_hz);
        } else {
            qDebug("XTRXMIMO::setRxDeviceCenterFrequency: Rx frequency set to %llu", freq_hz);
        }
    }
}

void XTRXMIMO::setTxDeviceCenterFrequency(xtrx_dev *dev, quint64 freq_hz, int loPpmTenths)
{
    if (dev)
    {
        if (xtrx_tune(dev,
                XTRX_TUNE_TX_FDD,
                freq_hz,
                0) < 0) {
            qCritical("XTRXMIMO::setTxDeviceCenterFrequency: could not set Tx frequency to %llu", freq_hz);
        } else {
            qDebug("XTRXMIMO::setTxDeviceCenterFrequency: Tx frequency set to %llu", freq_hz);
        }
    }
}

void XTRXMIMO::webapiFormatDeviceSettings(
    SWGSDRangel::SWGDeviceSettings& response,
    const XTRXMIMOSettings& settings)
{
    // TODO
    (void) response;
    (void) settings;
}

void XTRXMIMO::webapiUpdateDeviceSettings(
    XTRXMIMOSettings& settings,
    const QStringList& deviceSettingsKeys,
    SWGSDRangel::SWGDeviceSettings& response)
{
    // TODO
    (void) settings;
    (void) deviceSettingsKeys;
    (void) response;
}