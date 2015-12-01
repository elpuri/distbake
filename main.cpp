/*
-- Copyright (c) 2015, Juha Turunen (turunen@iki.fi)
-- All rights reserved.
--
-- Redistribution and use in source and binary forms, with or without
-- modification, are permitted provided that the following conditions are met:
--
-- 1. Redistributions of source code must retain the above copyright notice, this
--    list of conditions and the following disclaimer.
-- 2. Redistributions in binary form must reproduce the above copyright notice,
--    this list of conditions and the following disclaimer in the documentation
--    and/or other materials provided with the distribution.
--
-- THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
-- ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
-- WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
-- DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
-- ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
-- (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
-- LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
-- ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
-- (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
-- SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <QCoreApplication>
#include <QSvgRenderer>
#include <QImage>
#include <QPainter>
#include <QDebug>
#include <QElapsedTimer>
#include <QCommandLineParser>
#include <memory>
#include <math.h>
#include <thread>

using namespace std;

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    QCommandLineParser cmdLine;
    cmdLine.setApplicationDescription("distbake generates distance fields out of SVG images");
    cmdLine.addHelpOption();
    cmdLine.addOption(QCommandLineOption(
                          "sourcesize",
                          "The length of the longer edge of the image the SVG gets rasterized "
                          "to measured in pixels. A larger size produces higher quality "
                          "output, but increases processing time. The default value is 3000.",
                          "size", "3000"));
    cmdLine.addOption(QCommandLineOption(
                          "maxdist",
                          "The maximum distance measured in source image pixels which the "
                          "distance search will search for. Using a smaller value speeds up "
                          "the process, but produces a narrower gradient around outline, thus"
                          "limiting the usefulness in producing outline and shadow effects. Using a "
                          "too large value can cause problems with concave shapes with small "
                          "detail. The value should be scaled proportionally as sourcesize changes. "
                          "The values in the output image are mapped [-sqrt(2 * maxdist.. sqrt(2 * maxdist)] => [0.255]. "
                          "The default value is 8.",
                          "distance", "8"
                          ));
    cmdLine.addOption(QCommandLineOption(
                          "targetsize",
                          "The length of the longer edge of the distance field output. The smaller the "
                          "outputsize gets, the more detail is lost. Also when rendering sharp corners "
                          "aren't preserved if scaled larger than targetsize. By default the targetsize "
                          "is 1/16th of the sourcesize.",
                          "size"
                          ));
    cmdLine.addOption(QCommandLineOption(
                          QStringList() << "threads" << "t",
                          "Force the program to use a certain number of threads. By default the number is"
                          "the amount of hardware threads available on the CPU.",
                          "count"
                          ));
    cmdLine.addOption(QCommandLineOption(
                          "negate",
                          "By default the tool assumes that black (or darker than mid-gray) colors in "
                          "the source image are inside the shape. If negate option is given white (or "
                          "lighter than mid-gray) colors are assumed to be inside the shape."
                      ));
    cmdLine.addOption(QCommandLineOption(
                          "savesource",
                          "Save the source buffer used to generate the distance field as a PNG file "
                          "for debugging purposes.",
                          "filename"
                          ));
    cmdLine.addPositionalArgument("inputfile", "SVG input file");
    cmdLine.addPositionalArgument("outputfile", "PNG output file");
    cmdLine.parse(a.arguments());

    if (cmdLine.positionalArguments().count() < 2) {
        puts(qPrintable(cmdLine.helpText()));
        return 0;
    }

    QSvgRenderer svg(cmdLine.positionalArguments().at(0));
    if (!svg.isValid())
        return 0;

    QSize svgSize = svg.defaultSize();
    float aspect = (float) svgSize.width() / svgSize.height();

    bool ok;
    int md = cmdLine.value("maxdist").toInt(&ok);
    if (!ok || md < 1) {
        puts(qPrintable(cmdLine.helpText()));
        return 0;
    }

    int kernelDim = md * 2 + 1;

    // Setup search kernel look-up table
    int center = md;
    shared_ptr<float> searchKernel(new float[kernelDim * kernelDim]);

    int x = 0, y = 0;
    for (int i = 0; i < kernelDim * kernelDim; i++) {
        int dx = x - center; int dy = y - center;
        searchKernel.get()[i] = sqrt(dx * dx + dy *dy);
        x++;
        if ((i + 1)% kernelDim == 0) {
            y++;
            x = 0;
        }
    }

    float maxDist = sqrt(2 * center * center);

    int longDim = cmdLine.value("sourcesize").toInt();
    if (longDim < 1) {
        puts(qPrintable(cmdLine.helpText()));
        return 0;
    }

    QSize imageSize = aspect < 1.0 ? QSize(longDim * aspect, longDim) : QSize(longDim, longDim / aspect);
    qInfo("Rendering SVG to %dx%d", imageSize.width(), imageSize.height());
    QImage i(imageSize + QSize(kernelDim, kernelDim), QImage::Format_Grayscale8);

    bool negate = cmdLine.isSet("negate");
    i.fill(negate ? Qt::black : Qt::white);
    QPainter painter(&i);
    svg.render(&painter, QRectF(center, center, imageSize.width(), imageSize.height()));

    if (cmdLine.isSet("savesource"))
        i.save(cmdLine.value("savesource"), "png");

    QImage df(imageSize, QImage::Format_Grayscale8);

    QElapsedTimer elapsed;
    elapsed.start();

    int imageScanlineLength = i.scanLine(1) - i.scanLine(0);

    const float* kernelPtr = searchKernel.get();

    auto calculateDistance = [&](int interleave, int lineStride) {
        for (int y = interleave; y < imageSize.height(); y += lineStride) {
            const uchar* imageLine = i.constScanLine(y);
            uchar* fieldLine = df.scanLine(y);

            for (int x = 0; x < imageSize.width(); x++) {
                int kernelIndex = 0;
                bool inside = imageLine[x + center + center * imageScanlineLength] >= 128;
                float minDistance = 1e6;

                for (int j = 0; j < kernelDim; j++) {
                    for (int i = 0; i < kernelDim; i++) {
                        unsigned char px = imageLine[x + i + j * imageScanlineLength];
                        if ((inside && px < 128) || (!inside && px >= 128))
                            minDistance = min(kernelPtr[kernelIndex++], minDistance);
                    }
                }

                if (minDistance > maxDist)
                    minDistance = maxDist;

                if (inside)
                    minDistance = -minDistance;

                *fieldLine++ = ((minDistance / maxDist) + 1.0) * 0.5  * 255;
            }
        }
    };

    int numThreads;

    if (cmdLine.isSet("threads")) {
        numThreads = cmdLine.value("threads").toInt();
        if (numThreads < 1) {
            puts(qPrintable(cmdLine.helpText()));
            return 0;
        }
    } else {
        numThreads = thread::hardware_concurrency();
        if (numThreads == 0) {
            qInfo("Couldn't figure out the number of hardware threads. Defaulting to 4.");
            numThreads = 4;
        }
    }

    qInfo("Using %d threads", numThreads);
    vector<thread> threads;
    threads.reserve(numThreads);
    for (int threadId = 0; threadId < numThreads; threadId++) {
        threads.push_back(thread([=]() {
            calculateDistance(threadId, numThreads);
        }));
    }

    for (int threadId = 0; threadId < numThreads; threadId++)
        threads[threadId].join();

    if (negate)
        df.invertPixels();

    QString outputFilename = cmdLine.positionalArguments().at(1);
    QSize outputSize(imageSize / 16.0f);
    if (cmdLine.isSet("targetsize")) {
        int outputEdge = cmdLine.value("targetsize").toInt();
        if (outputEdge < 1) {
            puts(qPrintable(cmdLine.helpText()));
            return 0;
        }
        outputSize = aspect < 1.0 ? QSize(outputEdge * aspect, outputEdge) :
                                    QSize(outputEdge, outputEdge / aspect);
    }
    qInfo("Generated distance field of size %dx%d in %dms",
          outputSize.width(), outputSize.height(), (int) elapsed.elapsed());

    df = df.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    df.save(outputFilename, "png");
    qInfo("Saved %s", qPrintable(outputFilename));
}

