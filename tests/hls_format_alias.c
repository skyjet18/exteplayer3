#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#if LIBAVFORMAT_VERSION_MAJOR >= 61
/* Since FFmpeg 7 the demuxer callbacks live in FFInputFormat around the
 * public AVInputFormat, so the alias has to copy the whole structure. */
#include <libavformat/demux.h>
#endif

int __real_avformat_open_input(AVFormatContext **context, const char *url,
                              const AVInputFormat *format, AVDictionary **options);

int __wrap_avformat_open_input(AVFormatContext **context, const char *url,
                              const AVInputFormat *format, AVDictionary **options)
{
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static FFInputFormat alias;
#else
    static AVInputFormat alias;
#endif
    int result = __real_avformat_open_input(context, url, format, options);
    if (result >= 0 && av_match_name("hls", (*context)->iformat->name))
    {
#if LIBAVFORMAT_VERSION_MAJOR >= 61
        alias = *ffifmt((*context)->iformat);
        alias.p.name = "hls,applehttp";
        (*context)->iformat = &alias.p;
#else
        alias = *(*context)->iformat;
        alias.name = "hls,applehttp";
        (*context)->iformat = &alias;
#endif
    }
    return result;
}
