#include <libavformat/avformat.h>
#include <libavutil/avstring.h>

int __real_avformat_open_input(AVFormatContext **context, const char *url,
                              const AVInputFormat *format, AVDictionary **options);

int __wrap_avformat_open_input(AVFormatContext **context, const char *url,
                              const AVInputFormat *format, AVDictionary **options)
{
    static AVInputFormat alias;
    int result = __real_avformat_open_input(context, url, format, options);
    if (result >= 0 && av_match_name("hls", (*context)->iformat->name))
    {
        alias = *(*context)->iformat;
        alias.name = "hls,applehttp";
        (*context)->iformat = &alias;
    }
    return result;
}
