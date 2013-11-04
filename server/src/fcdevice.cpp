/*
 * Fadecandy device interface
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fcdevice.h"
#include "opc.h"
#include <math.h>
#include <iostream>
#include <sstream>
#include <stdio.h>


FCDevice::Transfer::Transfer(FCDevice *device, void *buffer, int length, PacketType type)
    : transfer(libusb_alloc_transfer(0)),
      type(type), finished(false)
{
    libusb_fill_bulk_transfer(transfer, device->mHandle,
        OUT_ENDPOINT, (uint8_t*) buffer, length, FCDevice::completeTransfer, this, 2000);
}

FCDevice::Transfer::~Transfer()
{
    libusb_free_transfer(transfer);
}

FCDevice::FCDevice(libusb_device *device, bool verbose)
    : USBDevice(device, verbose),
      mConfigMap(0), mNumFramesPending(0), mFrameWaitingForSubmit(false)
{
    mSerial[0] = '\0';

    memset(&mFirmwareConfig, 0, sizeof mFirmwareConfig);
    mFirmwareConfig.control = TYPE_CONFIG;

    // Framebuffer headers
    memset(mFramebuffer, 0, sizeof mFramebuffer);
    for (unsigned i = 0; i < FRAMEBUFFER_PACKETS; ++i) {
        mFramebuffer[i].control = TYPE_FRAMEBUFFER | i;
    }
    mFramebuffer[FRAMEBUFFER_PACKETS - 1].control |= FINAL;

    // Color LUT headers
    memset(mColorLUT, 0, sizeof mColorLUT);
    for (unsigned i = 0; i < LUT_PACKETS; ++i) {
        mColorLUT[i].control = TYPE_LUT | i;
    }
    mColorLUT[LUT_PACKETS - 1].control |= FINAL;
}

FCDevice::~FCDevice()
{
    /*
     * If we have pending transfers, cancel them.
     * The Transfer objects themselves will be freed
     * once libusb completes them.
     */

    for (std::set<Transfer*>::iterator i = mPending.begin(), e = mPending.end(); i != e; ++i) {
        Transfer *fct = *i;
        libusb_cancel_transfer(fct->transfer);
    }
}

bool FCDevice::probe(libusb_device *device)
{
    libusb_device_descriptor dd;

    if (libusb_get_device_descriptor(device, &dd) < 0) {
        // Can't access descriptor?
        return false;
    }

    return dd.idVendor == 0x1d50 && dd.idProduct == 0x607a;
}

int FCDevice::open()
{
    int r = libusb_get_device_descriptor(mDevice, &mDD);
    if (r < 0) {
        return r;
    }

    r = libusb_open(mDevice, &mHandle);
    if (r < 0) {
        return r;
    }

    r = libusb_claim_interface(mHandle, 0);
    if (r < 0) {
        return r;
    }

    return libusb_get_string_descriptor_ascii(mHandle, mDD.iSerialNumber, (uint8_t*)mSerial, sizeof mSerial);
}

bool FCDevice::matchConfiguration(const Value &config)
{
    if (matchConfigurationWithTypeAndSerial(config, "fadecandy", mSerial)) {
        mConfigMap = findConfigMap(config);
        configureDevice(config);
        return true;
    }

    return false;
}

void FCDevice::configureDevice(const Value &config)
{
    /*
     * Send a device configuration settings packet, using the default values in our
     * JSON config file. This can be overridden over OPC later on.
     */

    const Value &led = config["led"];

    if (!(led.IsTrue() || led.IsFalse() || led.IsNull())) {
        std::clog << "LED configuration must be true (always on), false (always off), or null (default).\n";
    }

    mFirmwareConfig.data[0] =
        (led.IsNull() ? 0 : CFLAG_NO_ACTIVITY_LED) |
        (led.IsTrue() ? CFLAG_LED_CONTROL : 0)     ;

    writeFirmwareConfiguration();
}

bool FCDevice::submitTransfer(Transfer *fct)
{
    /*
     * Submit a new USB transfer. The Transfer object is guaranteed to be freed eventually.
     * On error, it's freed right away.
     */

    int r = libusb_submit_transfer(fct->transfer);

    if (r < 0) {
        if (mVerbose && r != LIBUSB_ERROR_PIPE) {
            std::clog << "Error submitting USB transfer: " << libusb_strerror(libusb_error(r)) << "\n";
        }
        delete fct;
        return false;

    } else {
        mPending.insert(fct);
        return true;
    }
}

void FCDevice::completeTransfer(libusb_transfer *transfer)
{
    FCDevice::Transfer *fct = static_cast<FCDevice::Transfer*>(transfer->user_data);
    fct->finished = true;
}

void FCDevice::flush()
{
    // Erase any finished transfers

    std::set<Transfer*>::iterator current = mPending.begin();
    while (current != mPending.end()) {
        std::set<Transfer*>::iterator next = current;
        next++;

        Transfer *fct = *current;
        if (fct->finished) {
            switch (fct->type) {

                case FRAME:
                    mNumFramesPending--;
                    break;

                default:
                    break;
            }

            mPending.erase(current);
            delete fct;
        }

        current = next;
    }

    // Submit new frames, if we had a queued frame waiting

    if (mFrameWaitingForSubmit && mNumFramesPending < MAX_FRAMES_PENDING) {
        writeFramebuffer();
    }
}

void FCDevice::writeColorCorrection(const Value &color)
{
    /*
     * Populate the color correction table based on a JSON configuration object,
     * and send the new color LUT out over USB.
     *
     * 'color' may be 'null' to load an identity-mapped LUT, or it may be
     * a dictionary of options including 'gamma' and 'whitepoint'.
     *
     * This calculates a compound curve with a linear section and a nonlinear
     * section. The linear section, near zero, avoids creating very low output
     * values that will cause distracting flicker when dithered. This isn't a problem
     * when the LEDs are viewed indirectly such that the flicker is below the threshold
     * of perception, but in cases where the flicker is a problem this linear section can
     * eliminate it entierly at the cost of some dynamic range.
     *
     * By default, the linear section is disabled (linearCutoff is zero). To enable the
     * linear section, set linearCutoff to some nonzero value. A good starting point is
     * 1/256.0, correspnding to the lowest 8-bit PWM level.
     */

    // Default color LUT parameters
    double gamma = 1.0;                         // Power for nonlinear portion of curve
    double whitepoint[3] = {1.0, 1.0, 1.0};     // White-point RGB value (also, global brightness)
    double linearSlope = 1.0;                   // Slope (output / input) of linear section of the curve, near zero
    double linearCutoff = 0.0;                  // Y (output) coordinate of intersection of linear and nonlinear curves

    /*
     * Parse the JSON object
     */

    if (color.IsObject()) {
        const Value &vGamma = color["gamma"];
        const Value &vWhitepoint = color["whitepoint"];
        const Value &vLinearSlope = color["linearSlope"];
        const Value &vLinearCutoff = color["linearCutoff"];

        if (vGamma.IsNumber()) {
            gamma = vGamma.GetDouble();
        } else if (!vGamma.IsNull() && mVerbose) {
            std::clog << "Gamma value must be a number.\n";
        }

        if (vLinearSlope.IsNumber()) {
            linearSlope = vLinearSlope.GetDouble();
        } else if (!vLinearSlope.IsNull() && mVerbose) {
            std::clog << "Linear slope value must be a number.\n";
        }

        if (vLinearCutoff.IsNumber()) {
            linearCutoff = vLinearCutoff.GetDouble();
        } else if (!vLinearCutoff.IsNull() && mVerbose) {
            std::clog << "Linear slope value must be a number.\n";
        }

        if (vWhitepoint.IsArray() &&
            vWhitepoint.Size() == 3 &&
            vWhitepoint[0u].IsNumber() &&
            vWhitepoint[1].IsNumber() &&
            vWhitepoint[2].IsNumber()) {
            whitepoint[0] = vWhitepoint[0u].GetDouble();
            whitepoint[1] = vWhitepoint[1].GetDouble();
            whitepoint[2] = vWhitepoint[2].GetDouble();
        } else if (!vWhitepoint.IsNull() && mVerbose) {
            std::clog << "Whitepoint value must be a list of 3 numbers.\n";
        }

    } else if (!color.IsNull() && mVerbose) {
        std::clog << "Color correction value must be a JSON dictionary object.\n";
    }

    /*
     * Calculate the color LUT, stowing the result in an array of USB packets.
     */

    Packet *packet = mColorLUT;
    const unsigned firstByteOffset = 1;  // Skip padding byte
    unsigned byteOffset = firstByteOffset;

    for (unsigned channel = 0; channel < 3; channel++) {
        for (unsigned entry = 0; entry < LUT_ENTRIES; entry++) {
            double output;

            /*
             * Normalized input value corresponding to this LUT entry.
             * Ranges from 0 to slightly higher than 1. (The last LUT entry
             * can't quite be reached.)
             */
            double input = (entry << 8) / 65535.0;

            // Scale by whitepoint before anything else
            input *= whitepoint[channel];

            // Is this entry part of the linear section still?
            if (input * linearSlope <= linearCutoff) {

                // Output value is below linearCutoff. We're still in the linear portion of the curve
                output = input * linearSlope;

            } else {

                // Nonlinear portion of the curve. This starts right where the linear portion leaves
                // off. We need to avoid any discontinuity.

                double nonlinearInput = input - (linearSlope * linearCutoff);
                double scale = 1.0 - linearCutoff;
                output = linearCutoff + pow(nonlinearInput / scale, gamma) * scale;
            }

            // Round to the nearest integer, and clamp. Overflow-safe.
            int64_t longValue = (output * 0xFFFF) + 0.5;
            int intValue = std::max<int64_t>(0, std::min<int64_t>(0xFFFF, longValue));

            // Store LUT entry, little-endian order.
            packet->data[byteOffset++] = uint8_t(intValue);
            packet->data[byteOffset++] = uint8_t(intValue >> 8);
            if (byteOffset >= sizeof packet->data) {
                byteOffset = firstByteOffset;
                packet++;
            }
        }
    }

    // Start asynchronously sending the LUT.
    submitTransfer(new Transfer(this, &mColorLUT, sizeof mColorLUT));
}

void FCDevice::writeFramebuffer()
{
    /*
     * Asynchronously write the current framebuffer.
     * Note that the OS will copy our framebuffer at submit-time.
     *
     * TODO: Currently if this gets ahead of what the USB device is capable of,
     *       we always drop frames. Alternatively, it would be nice to have end-to-end
     *       flow control so that the client can produce frames slower.
     */

    if (mNumFramesPending >= MAX_FRAMES_PENDING) {
        // Too many outstanding frames. Wait to submit until a previous frame completes.
        mFrameWaitingForSubmit = true;
        return;
    }

    if (submitTransfer(new Transfer(this, &mFramebuffer, sizeof mFramebuffer, FRAME))) {
        mFrameWaitingForSubmit = false;
        mNumFramesPending++;
    }
}

void FCDevice::writeMessage(const OPC::Message &msg)
{
    /*
     * Dispatch an incoming OPC command
     */

    switch (msg.command) {

        case OPC::SetPixelColors:
            opcSetPixelColors(msg);
            writeFramebuffer();
            return;

        case OPC::SystemExclusive:
            opcSysEx(msg);
            return;
    }

    if (mVerbose) {
        std::clog << "Unsupported OPC command: " << unsigned(msg.command) << "\n";
    }
}

void FCDevice::opcSysEx(const OPC::Message &msg)
{
    if (msg.length() < 4) {
        if (mVerbose) {
            std::clog << "SysEx message too short!\n";
        }
        return;
    }

    unsigned id = (unsigned(msg.data[0]) << 24) |
                  (unsigned(msg.data[1]) << 16) |
                  (unsigned(msg.data[2]) << 8)  |
                   unsigned(msg.data[3])        ;

    switch (id) {

        case OPC::FCSetGlobalColorCorrection:
            return opcSetGlobalColorCorrection(msg);

        case OPC::FCSetFirmwareConfiguration:
            return opcSetFirmwareConfiguration(msg);

    }

    // Quietly ignore unhandled SysEx messages.
}

void FCDevice::opcSetPixelColors(const OPC::Message &msg)
{
    /*
     * Parse through our device's mapping, and store any relevant portions of 'msg'
     * in the framebuffer.
     */

    if (!mConfigMap) {
        // No mapping defined yet. This device is inactive.
        return;
    }

    const Value &map = *mConfigMap;
    for (unsigned i = 0, e = map.Size(); i != e; i++) {
        opcMapPixelColors(msg, map[i]);
    }
}

void FCDevice::opcMapPixelColors(const OPC::Message &msg, const Value &inst)
{
    /*
     * Parse one JSON mapping instruction, and copy any relevant parts of 'msg'
     * into our framebuffer. This looks for any mapping instructions that we
     * recognize:
     *
     *   [ OPC Channel, First OPC Pixel, First output pixel, pixel count ]
     */

    unsigned msgPixelCount = msg.length() / 3;

    if (inst.IsArray() && inst.Size() == 4) {
        // Map a range from an OPC channel to our framebuffer

        const Value &vChannel = inst[0u];
        const Value &vFirstOPC = inst[1];
        const Value &vFirstOut = inst[2];
        const Value &vCount = inst[3];

        if (vChannel.IsUint() && vFirstOPC.IsUint() && vFirstOut.IsUint() && vCount.IsUint()) {
            unsigned channel = vChannel.GetUint();
            unsigned firstOPC = vFirstOPC.GetUint();
            unsigned firstOut = vFirstOut.GetUint();
            unsigned count = vCount.GetUint();

            if (channel != msg.channel) {
                return;
            }

            // Clamping, overflow-safe
            firstOPC = std::min<unsigned>(firstOPC, msgPixelCount);
            firstOut = std::min<unsigned>(firstOut, unsigned(NUM_PIXELS));
            count = std::min<unsigned>(count, msgPixelCount - firstOPC);
            count = std::min<unsigned>(count, NUM_PIXELS - firstOut);

            // Copy pixels
            const uint8_t *inPtr = msg.data + (firstOPC * 3);
            unsigned outIndex = firstOut;

            while (count--) {
                uint8_t *outPtr = fbPixel(outIndex++);
                outPtr[0] = inPtr[0];
                outPtr[1] = inPtr[1];
                outPtr[2] = inPtr[2];
                inPtr += 3;
            }

            return;
        }
    }

    // Still haven't found a match?
    if (mVerbose) {
        std::clog << "Unsupported JSON mapping instruction\n";
    }
}

void FCDevice::opcSetGlobalColorCorrection(const OPC::Message &msg)
{
    /*
     * Parse the message as JSON text, and if successful, write new
     * color correction data to the device.
     */

    // Mutable NUL-terminated copy of the message string
    std::string text((char*)msg.data + 4, msg.length() - 4);

    // Parse it in-place
    rapidjson::Document doc;
    doc.ParseInsitu<0>(&text[0]);

    if (doc.HasParseError()) {
        if (mVerbose) {
            std::clog << "Parse error in color correction JSON at character "
                << doc.GetErrorOffset() << ": " << doc.GetParseError() << "\n";
        }
        return;
    }

    /*
     * Successfully parsed the JSON. From here, it's handled identically to
     * objects that come through the config file.
     */
    writeColorCorrection(doc);
}

void FCDevice::opcSetFirmwareConfiguration(const OPC::Message &msg)
{
    /*
     * Raw firmware configuration packet
     */

    memcpy(mFirmwareConfig.data, msg.data + 4, std::min<size_t>(sizeof mFirmwareConfig.data, msg.length() - 4));
    writeFirmwareConfiguration();
}

void FCDevice::writeFirmwareConfiguration()
{
    // Write mFirmwareConfig to the device
    submitTransfer(new Transfer(this, &mFirmwareConfig, sizeof mFirmwareConfig));
}

std::string FCDevice::getName()
{
    std::ostringstream s;
    s << "Fadecandy";
    if (mSerial[0]) {
        unsigned major = mDD.bcdDevice >> 8;
        unsigned minor = mDD.bcdDevice & 0xFF;
        char version[10];
        snprintf(version, sizeof version, "%x.%02x", major, minor);

        s << " (Serial# " << mSerial << ", Version " << version << ")";
    }
    return s.str();
}
