// SPDX-FileCopyrightText: 2022 Humanoid Sensing and Perception, Istituto Italiano di Tecnologia
// SPDX-License-Identifier: BSD-3-Clause
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>

#include <iostream>
#include "Detector.h"

YARP_LOG_COMPONENT(VADAUDIOPROCESSOR, "behavior_tour_robot.voiceActivationDetection.AudioProcessor", yarp::os::Log::TraceType)

Detector::Detector(int vadFrequency,
                               int vadSampleLength,
                               int vadAggressiveness,
                               int bufferSize,
                               std::string filteredAudioPortOutName):
                               m_vadFrequency(vadFrequency),
                               m_vadSampleLength(vadSampleLength),
                               m_vadAggressiveness(vadAggressiveness),
                               m_bufferSize(bufferSize),
                               m_filteredAudioPortOutName(filteredAudioPortOutName){

    m_fvadObject = fvad_new();
    if (!m_fvadObject)
    {
        yCError(VADAUDIOPROCESSOR) << "Failed to created VAD object";
    }
    /*
     * Changes the VAD operating ("aggressiveness") mode of a VAD instance.
     *
     * A more aggressive (higher mode) VAD is more restrictive in reporting speech.
     * Put in other words the probability of being speech when the VAD returns 1 is
     * increased with increasing mode. As a consequence also the missed detection
     * rate goes up.
     *
     * Valid modes are 0 ("quality"), 1 ("low bitrate"), 2 ("aggressive"), and 3
     * ("very aggressive"). The default mode is 0.
     *
     * Returns 0 on success, or -1 if the specified mode is invalid.
     */

    yCDebug(VADAUDIOPROCESSOR) << "m_vadAggressiveness" << m_vadAggressiveness;
    fvad_set_mode(m_fvadObject, m_vadAggressiveness);

    /*
     * Sets the input sample rate in Hz for a VAD instance.
     *
     * Valid values are 8000, 16000, 32000 and 48000. The default is 8000. Note
     * that internally all processing will be done 8000 Hz; input data in higher
     * sample rates will just be downsampled first.
     *
     * Returns 0 on success, or -1 if the passed value is invalid.
     */
     
    if (fvad_set_sample_rate(m_fvadObject, m_vadFrequency))
    {
        yCError(VADAUDIOPROCESSOR) << "Unsupported input frequency.";
    }

    if (!m_filteredAudioOutputPort.open(m_filteredAudioPortOutName)){
        yCError(VADAUDIOPROCESSOR) << "cannot open port" << m_filteredAudioPortOutName;
    }

    m_vadSampleLength = m_vadSampleLength * (m_vadFrequency / 1000); // from time to number of samples
    m_currentSoundBuffer = std::vector<int16_t>(m_vadSampleLength, 0);
    m_fillCount = 0;

    m_rpcClientPort.open("/vad/rpc:o");
    m_rpcClient.yarp().attachAsClient(m_rpcClientPort);
}


void Detector::onRead(yarp::sig::Sound& soundReceived) {
    size_t num_samples = soundReceived.getSamples();
    std::cout << soundReceived.toString() << std::endl;

    for (size_t i = 0; i < num_samples; i++)
    {
        m_currentSoundBuffer.at(m_fillCount) = soundReceived.get(i);
        ++m_fillCount;
        if (m_fillCount == m_vadSampleLength-1) {
            processPacket();
            m_fillCount = 0;
        }
    } 
    
}


void Detector::processPacket() {
    /*
    * Calculates a VAD decision for an audio frame.
    *
    * `frame` is an array of `length` signed 16-bit samples. Only frames with a
    * length of 10, 20 or 30 ms are supported, so for example at 8 kHz, `length`
    * must be either 80, 160 or 240.
    *
    * Returns              : 1 - (active voice),
    *                        0 - (non-active Voice),
    *                       -1 - (invalid frame length).
    */
    int isTalking = fvad_process(m_fvadObject, m_currentSoundBuffer.data(), m_currentSoundBuffer.size());

    if (isTalking < 0)
    {
        yCWarning(VADAUDIOPROCESSOR) << "Invalid frame length.";
    } else if (isTalking == 0) { 
        if (m_soundDetected)
        {
            ++m_gapCounter;
            if (m_gapCounter > m_gapAllowance)
            {
                yCDebug(VADAUDIOPROCESSOR) << "End of of speech";
                sendSound();
                m_soundToSend.clear();
                m_soundDetected = false;
                m_runInference = false; // stop detecting voice activity until wake work is invoked
                m_rpcClient.stop();
            }
        }   
    } else {
        yCDebug(VADAUDIOPROCESSOR) << "Voice detected adding to send buffer";
        m_soundDetected = true;
        m_soundToSend.push_back(m_currentSoundBuffer);
    }
}

void Detector::sendSound() {
    m_soundDetected = false;
    m_paddingCurrentSize = 0;
    int numberOfSamplesPerPacket = m_vadSampleLength * (m_vadFrequency / 1000);
    int numberOfPackets = m_soundToSend.size();

    yarp::sig::Sound& soundToSend = m_filteredAudioOutputPort.prepare();
    yCDebug(VADAUDIOPROCESSOR) << ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> sending recorded voice sound of " << numberOfPackets * m_currentSoundBuffer.size() <<   " samples";

    soundToSend.resize(m_currentSoundBuffer.size() * numberOfPackets);
    soundToSend.setFrequency(m_vadFrequency);
    for (size_t p = 0; p < numberOfPackets; ++p)
    {
        for (size_t i = 0; i < m_currentSoundBuffer.size(); i++)
        {
            soundToSend.set(m_soundToSend[p].at(i), i + (p * m_currentSoundBuffer.size()));
        }
    }
    
    m_filteredAudioOutputPort.write();
}

