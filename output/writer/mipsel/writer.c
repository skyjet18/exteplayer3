/*
 * linuxdvb output/writer handling.
 *
 * konfetti 2010
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* ***************************** */
/* Includes                      */
/* ***************************** */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "misc.h"
#include "writer.h"
#include "common.h"
#include "debug.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */
#define getDVBMutex(pmtx) do { if (pmtx) pthread_mutex_lock(pmtx);} while(false);
#define releaseDVBMutex(pmtx) do { if (pmtx) pthread_mutex_unlock(pmtx);} while(false);

/* longest wait of writev_with_retry() for room in a full device without a
 * sign from it: a safety net, room and WriterWakeUp() end the wait earlier */
#define WRITER_WAIT_MS 100

/* ***************************** */
/* Types                         */
/* ***************************** */
typedef enum {
    DVB_STS_UNKNOWN, 
    DVB_STS_SEEK, 
    DVB_STS_PAUSE, 
    DVB_STS_EXIT
} DVBState_t; 

/* ***************************** */
/* Varaibles                     */
/* ***************************** */

static Writer_t * AvailableWriter[] = {
    &WriterAudioAAC,
    &WriterAudioAACLATM,
    &WriterAudioAACPLUS,
    &WriterAudioAC3,
    &WriterAudioEAC3,
    &WriterAudioMP3,
    &WriterAudioMPEGL3,
    &WriterAudioPCM,
    &WriterAudioIPCM,
    &WriterAudioLPCM,
    &WriterAudioDTS,
    &WriterAudioWMA,
    &WriterAudioWMAPRO,
    &WriterAudioOPUS,
    &WriterAudioVORBIS,

    &WriterVideoH264,
    &WriterVideoH265,
    &WriterVideoH263,
    &WriterVideoMPEG4,
    &WriterVideoMPEG2,
    &WriterVideoMPEG1,
    &WriterVideoVC1,
    &WriterVideoDIVX3,
    &WriterVideoVP6,
    &WriterVideoVP8,
    &WriterVideoVP9,
    &WriterVideoFLV,
    &WriterVideoWMV,
    &WriterVideoMJPEG,
    &WriterVideoRV40,
    &WriterVideoRV30,
#ifdef HAVE_DREAMNEXTGEN
    &WriterVideoAVS,
#endif
    &WriterVideoAVS2,
    NULL
};

/* the wake-up pipe of writev_with_retry(), see WriterWakeUp() */
static int writerWakePipe[2] = {-1, -1};
static pthread_once_t writerWakeOnce = PTHREAD_ONCE_INIT;

/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/*  Functions                    */
/* ***************************** */
ssize_t WriteWithRetry(Context_t *context, int pipefd, int fd, void *pDVBMtx, const void *buf, int size, int is_video)
{
    fd_set rfds;
    fd_set wfds;

    ssize_t ret;
    int retval = -1;
    int maxFd = pipefd > fd ? pipefd : fd;
    struct timeval tv;
    const bool writeFirst = STB_HISILICON == GetSTBType();

    while(size > 0 && 0 == PlaybackDieNow(0) && !context->playback->isSeeking &&
          (is_video || !context->playback->isAudioSwitching))
    {
        if (writeFirst)
        {
            /* The devices of these boxes may never report that they are
             * writable, so do not wait for that: write, and only when the
             * device takes nothing wait for it or for the wake-up pipe,
             * 50 ms at most. */
            ret = write(fd, buf, size);
            if (ret > 0)
            {
                size -= ret;
                buf += ret;
                continue;
            }
            if (ret < 0 && errno != EINTR && errno != EAGAIN)
            {
                /* a hard error of the device: the caller drops the rest */
                writer_err("write failed: %s\n", strerror(errno));
                return -1;
            }

            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(pipefd, &rfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = 0;
            tv.tv_usec = 50000; // 50ms
            retval = select(maxFd + 1, &rfds, &wfds, NULL, &tv);
            if (retval < 0 && errno != EINTR)
            {
                writer_err("select failed: %s\n", strerror(errno));
                return -1;
            }
            if (retval > 0 && FD_ISSET(pipefd, &rfds))
            {
                FlushPipe(pipefd);
            }
            continue;
        }

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        FD_SET(pipefd, &rfds);
        FD_SET(fd, &wfds);

        /* When we PAUSE LINUX DVB outputs buffers, then audio/video buffers 
         * will continue to be filled. Unfortunately, in such case after resume 
         * select() will never return with fd set - bug in DVB drivers?
         * There are to workarounds possible:
         *   1. write to pipe at resume to return from select() immediately
         *   2. make timeout select(), limit max time spend in the select()
         *      to for example 0,1s 
         *   (at now first workaround is used)
         */
        //tv.tv_sec = 0;
        //tv.tv_usec = 100000; // 100ms
        
        retval = select(maxFd + 1, &rfds, &wfds, NULL, NULL); //&tv);
        if (retval < 0)
        {
            if (errno == EINTR)
            {
                /* interrupted by a signal: wait again for the rest */
                continue;
            }
            writer_err("select failed: %s\n", strerror(errno));
            return -1;
        }
        
        //if (retval == 0)
        //{
        //    //printf("RETURN FROM SELECT DUE TO TIMEOUT\n");
        //    continue;
        //}
        
        if(FD_ISSET(pipefd, &rfds))
        {
            FlushPipe(pipefd);
            //printf("RETURN FROM SELECT DUE TO pipefd SET\n");
            continue;
        }
        
        if(FD_ISSET(fd, &wfds))
        {
            ret = write(fd, buf, size);
            if (ret < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                {
                    continue;
                }
                /* a hard error of the device: the caller drops the rest */
                writer_err("write failed: %s\n", strerror(errno));
                return -1;
            }
            else if (ret == 0)
            {
                // printf("This should not happen. Select return fd ready to write, but write return 0, errno [%d]\n", errno);
                // wait 10ms before next try
                tv.tv_sec = 0;
                tv.tv_usec = 10000; // 10ms
                retval = select(pipefd + 1, &rfds, NULL, NULL, &tv);
                if (retval)
                    FlushPipe(pipefd);
                continue;
            }
            
            size -= ret;
            buf += ret;
        }
    }
    return 0;
}

static void WriterWakeInit(void)
{
    int i;
    if (pipe(writerWakePipe) != 0)
    {
        writer_err("wake-up pipe: %s\n", strerror(errno));
        writerWakePipe[0] = writerWakePipe[1] = -1;
        return;
    }
    for (i = 0; i < 2; i++)
    {
        int flags = fcntl(writerWakePipe[i], F_GETFL);
        if (flags == -1 || fcntl(writerWakePipe[i], F_SETFL, flags | O_NONBLOCK) == -1)
            writer_err("wake-up pipe: %s\n", strerror(errno));
        fcntl(writerWakePipe[i], F_SETFD, FD_CLOEXEC);
    }
    /* a stop ends the wait at once */
    PlaybackDieNowRegisterCallback(WriterWakeUp);
}

/* Ends a wait of writev_with_retry() for room in the device: the output calls
 * it at a resume, a clear and a stop. Some drivers do not report the device
 * writable again after a resume (see WriteWithRetry). */
void WriterWakeUp(void)
{
    pthread_once(&writerWakeOnce, WriterWakeInit);
    /* a full pipe has a wake-up pending already */
    if (writerWakePipe[1] >= 0 && write(writerWakePipe[1], "x", 1) != 1)
        writer_printf(20, "wake-up pending\n");
}

/* The device took nothing: wait in poll() until it has room, WriterWakeUp()
 * or WRITER_WAIT_MS, instead of retrying every millisecond, which kept a
 * paused player busy. *writable is set when poll() reported the device
 * writable: a device that does so and still takes nothing, and the HiSilicon
 * devices that may never report it, get the 1 ms retry of before. */
static void WaitForRoom(int fd, int *writable)
{
    struct pollfd pfd[2];
    int n = 1, ret;
    char tmp;

    if (*writable || STB_HISILICON == GetSTBType())
    {
        *writable = 0;
        usleep(1000);
        return;
    }
    pthread_once(&writerWakeOnce, WriterWakeInit);
    pfd[0].fd = fd;
    pfd[0].events = POLLOUT;
    pfd[0].revents = 0;
    if (writerWakePipe[0] >= 0)
    {
        pfd[1].fd = writerWakePipe[0];
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        n = 2;
    }
    ret = poll(pfd, n, WRITER_WAIT_MS);
    if (ret < 0)
    {
        if (errno != EINTR)
            usleep(1000);
        return;
    }
    if (n == 2 && (pfd[1].revents & POLLIN))
    {
        while (1 == read(writerWakePipe[0], &tmp, 1));
    }
    *writable = ret > 0 && (pfd[0].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) != 0;
}

ssize_t writev_with_retry(int fd, const struct iovec *iov, int ic)
{
    ssize_t len = 0;
    int index = 0;
    int writable = 0;
    size_t offset = 0;
    if (ic < 0 || (ic && !iov))
    {
        errno = EINVAL;
        writer_err("invalid scatter/gather vector\n");
        return -1;
    }
    while (index < ic)
    {
        struct iovec batch[64];
        int n, count = ic - index < 64 ? ic - index : 64;
        ssize_t written;
        if (PlaybackDieNow(0))
        {
            errno = ECANCELED;
            return -1;
        }
        for (n = 0; n < count; n++)
            batch[n] = iov[index + n];
        batch[0].iov_base = (char *)batch[0].iov_base + offset;
        batch[0].iov_len -= offset;
        written = writev(fd, batch, count);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN)
            {
                WaitForRoom(fd, &writable);
                continue;
            }
            writer_err("writev failed: %s\n", strerror(errno));
            return -1;
        }
        if (!written)
        {
            WaitForRoom(fd, &writable);
            continue;
        }
        writable = 0;
        if (written > SSIZE_MAX - len)
        {
            errno = EOVERFLOW;
            writer_err("scatter/gather byte count overflow\n");
            return -1;
        }
        len += written;
        while (index < ic && (size_t)written >= iov[index].iov_len - offset)
        {
            written -= iov[index].iov_len - offset;
            offset = 0;
            index++;
        }
        offset += written;
    }
    return len;
}

Writer_t* getWriter(char* encoding)
{
    int i;

    for (i = 0; AvailableWriter[i] != NULL; i++)
    {
        if (strcmp(AvailableWriter[i]->caps->textEncoding, encoding) == 0)
        {
            writer_printf(50, "%s: found writer \"%s\" for \"%s\"\n", __func__, AvailableWriter[i]->caps->name, encoding);
            return AvailableWriter[i];
        }
    }

    writer_printf(1, "%s: no writer found for \"%s\"\n", __func__, encoding);

    return NULL;
}

Writer_t* getDefaultVideoWriter()
{
    int i;

    for (i = 0; AvailableWriter[i] != NULL; i++)
    {
        if (strcmp(AvailableWriter[i]->caps->textEncoding, "V_MPEG2") == 0)
        {
            writer_printf(50, "%s: found writer \"%s\"\n", __func__, AvailableWriter[i]->caps->name);
            return AvailableWriter[i];
        }
    }

    writer_printf(1, "%s: no writer found\n", __func__);

    return NULL;
}

Writer_t* getDefaultAudioWriter()
{
    int i;

    for (i = 0; AvailableWriter[i] != NULL; i++)
    {
        if (strcmp(AvailableWriter[i]->caps->textEncoding, "A_MP3") == 0)
        {
            writer_printf(50, "%s: found writer \"%s\"\n", __func__, AvailableWriter[i]->caps->name);
            return AvailableWriter[i];
        }
    }

    writer_printf(1, "%s: no writer found\n", __func__);

    return NULL;
}
