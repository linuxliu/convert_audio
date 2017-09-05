/**
 * @file
 * audio_file ----->>> demux  ----->>> codec ----->>> filter ----->>> fifo ----->>> encode ----->>> mux
 *
 * @author liu_hq
 */
#include <ctype.h>
#include <string.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/timestamp.h>
#include <signal.h>

#define INPUT_DEVICE_FORMAT_NAME "avfoundation"
#define INPUT_DEVICE_NAME ":0"

#define FILTER_BUFFER_SRC_NAME "test_src"
#define FILTER_BUFFER_SINK_NAME "test_sink"
#define LOG_BUFFER_LEN 1024

// #define AV_LOG_FATAL     8
// #define AV_LOG_ERROR    16
// #define AV_LOG_WARNING  24
// #define AV_LOG_WARNING     32
// #define AV_LOG_DEBUG    48

typedef struct OutputCodecPara_st{
    int sample_rate;
    int channels;
    enum AVSampleFormat sample_fmt;
    int64_t bit_rate;
    uint64_t channel_layout;
    char output_format_name[128];
    char codec_name[128];
} OutputCodecPara;

static OutputCodecPara global_para = {
    .bit_rate = 64000,
    .sample_rate = 44100,
    .channels = 2,
    .sample_fmt = AV_SAMPLE_FMT_S16,
    .channel_layout = AV_CH_LAYOUT_STEREO,
    .output_format_name = "ssegment",
    .codec_name = "libfdk_aac",
};

static int global_log_level = AV_LOG_QUIET;

static void (*print_log)(const char*) = NULL;
static void (*tsComplateCB)(const char *path, int is_end) = NULL;

static AVDictionary *input_format_options = NULL;
static AVDictionary *input_codec_options = NULL;
static AVDictionary *output_codec_options = NULL;
static AVDictionary *output_format_options = NULL;

static double request_duration = 0.0;
static int abort_flag = 0;
static int tsIndex = 0;
static double ts_duration = 2.0;
static char outFile[LOG_BUFFER_LEN] = { 0 };

AVStream *outputStream = NULL;

static void signalHandler( int signum)
{
    if (signum == SIGTERM) {
        abort_flag = 1;
        av_log(NULL, AV_LOG_ERROR, "signalHandler SIGTERM change abort_flag to %d", abort_flag);
    }
}

/**
 * @brief 设置TS完成回调函数
 * @return 1 failed 0 success
 */
int setTsComplateCB(void (*tsCB)(const char *path, int is_end))
{
    if (NULL == tsCB) {
        return 1;
    }
    else {
        tsComplateCB = tsCB;
        return 0;
    }
}

static double getTsDuration()
{
    double ret = 0.0;
    if (NULL != output_format_options) {
        AVDictionaryEntry* entry = av_dict_get(output_format_options, "segment_time", NULL,AV_DICT_IGNORE_SUFFIX);
        if (NULL != entry) {
            ret = atof(entry->value);
        }
    }
    return ret;
}

/**
 * @brief 监控AVpacket 的dts
 * @param fmt_ctx
 * @param pkt
 */
static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    double current = atof(av_ts2timestr(pkt->dts, time_base));
    if (current >= (tsIndex + 1) * ts_duration) {
        tsIndex++;
        char temp[LOG_BUFFER_LEN] = { 0 };
        snprintf(temp, LOG_BUFFER_LEN, outFile, tsIndex-1);
        if (NULL != tsComplateCB) {
            tsComplateCB(temp,0);
        }
    }
}

/**
 * @brief do_abort
 * @param para
 * @return
 */
static int do_abort(void * para)
{
    if (abort_flag) {
        av_log(NULL, AV_LOG_ERROR, "do_abort AVFormat request abort");
        return 1;
    }
    else {
        return 0;
    }
}

static AVIOInterruptCB interrupt_callback = {
    do_abort,
    NULL,
};


/**
 * @brief set_transcode_duration
 * @param duration
 */
void set_transcode_duration(double duration)
{
    request_duration = duration;
}

/**
 * @brief set_abort_flag
 */
void set_abort_flag()
{
    av_log(NULL, AV_LOG_WARNING, "set_abort_flag ");
    if (!abort_flag) {
        abort_flag = 1;
    }
}

/**
 * @brief set_config_options
 * @param type
 * @param options_str
 * @return
 */
int set_config_options(const char* type, const char* options_str)
{
    if (NULL == type) {
        return 0;
    }
    int len = 0;
    if (NULL == options_str ||
            (0 == (len = strlen(options_str)))) {
        return 0;
    }
    av_log(NULL, AV_LOG_WARNING, "option type is %s : options : %s", type, options_str);
    char *input = (char*)malloc(len + 2);
    if (NULL == input) {
        return -1;
    }
    char *p = (char*)options_str;
    char *q = input;
    while (*p != '\0') {
        if (!isspace(*p)) {
            *q++ = *p++;
        }
        else {
            p++;
            continue;
        }
    }

    if (*(q - 1) != '|') {
        *q = '|';
        *(q + 1) = '\0';
    }
    else {
        *q = '\0';
    }

    q = NULL;
    p = NULL;
    char *m = input;
    av_log(NULL, AV_LOG_WARNING, " %d %s",atoi(type), input);
    while ((p = strchr(m, '|')) != NULL) {
        *p = '\0';
        q = strchr(m, ':');
        if (NULL != q) {
            *q = '\0';
            switch(atoi(type)) {
            case 1:
                av_dict_set(&input_format_options, m, q + 1, 0);
                break;
            case 2:
                av_dict_set(&input_codec_options, m, q + 1, 0);
                break;
            case 3:
                av_dict_set(&output_codec_options, m, q + 1, 0);
                break;
            case 4:
                av_dict_set(&output_format_options, m, q + 1, 0);
                break;
            default:
                free(input);
                input = NULL;
                return -1;
            }
            *q = ':';
        }
        *p = '|';
        m = p + 1;
    }
    free(input);
    input = NULL;
    return 0;
}

void set_output_para(OutputCodecPara *para)
{
    memcpy(&global_para, para, sizeof(OutputCodecPara));
}

void set_global_log_level(int level)
{
    global_log_level = level;
}

void set_global_log_print(void (*custom_log_print)(const char*))
{
    print_log = custom_log_print;
}

/**
 * @brief custom_log_callback
 * @param ptr
 * @param level
 * @param fmt
 * @param vl
 */
static void custom_log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level <= global_log_level) {
        if (NULL == print_log) {
            return;
        }
        else {
            char line[LOG_BUFFER_LEN] = {0};
            vsprintf(line, fmt, vl);
            print_log(line);
        }
    }
}

/**
 * @brief init_filters
 * @param filters_descr
 * @param input_codec_ctx
 * @param output_codec_ctx
 * @param filter_graph
 * @return
 */

static int init_filters(const char *filters_descr,
                        AVCodecContext *input_codec_ctx,
                        AVCodecContext *output_codec_ctx,
                        const AVFilterGraph **filter_graph)
{
    if (NULL == filters_descr) {
        av_log(NULL, AV_LOG_ERROR, "init_filters filters_descr NULL Error");
        return AVERROR_EXIT;
    }
    av_log(NULL, AV_LOG_WARNING, "init_filters filters_descr %s", filters_descr);

    int ret = 0;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();

    if (NULL == outputs || NULL == inputs) {
        av_log(NULL, AV_LOG_ERROR, "avfilter_inout_alloc error");
        return AVERROR(ENOMEM);
    }

    AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    if (NULL == abuffersrc || NULL == abuffersink) {
        av_log(NULL, AV_LOG_ERROR, "avfilter_get_by_name error");
        ret = AVERROR_EXIT;
        goto end;
    }

    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
//    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;
    AVRational time_base = input_codec_ctx->time_base;


    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (!input_codec_ctx->channel_layout) {
        input_codec_ctx->channel_layout = av_get_default_channel_layout(input_codec_ctx->channels);
    }

    AVFilterGraph * graph = avfilter_graph_alloc();
    if (NULL == graph) {
        av_log(NULL, AV_LOG_WARNING, "avfilter_graph_alloc Error");
        ret = AVERROR_EXIT;
        goto end;
    }

    char args[512] = { 0 };
    snprintf(args, sizeof(args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%llu",
             time_base.num, time_base.den, input_codec_ctx->sample_rate,
             av_get_sample_fmt_name(input_codec_ctx->sample_fmt), input_codec_ctx->channel_layout);
    av_log(NULL, AV_LOG_WARNING, "\nargs : %s\n", args);
    ret = avfilter_graph_create_filter(&buffersrc_ctx, abuffersrc, FILTER_BUFFER_SRC_NAME,
                                       args, NULL, graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source");
        avfilter_graph_free(&graph);
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, abuffersink, FILTER_BUFFER_SINK_NAME,
                                       NULL, NULL, graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink");
        avfilter_graph_free(&graph);
        goto end;
    }


    enum AVSampleFormat out_sample_fmts[] = {output_codec_ctx->sample_fmt, AV_SAMPLE_FMT_NONE};
    int64_t out_channel_layouts[] = {output_codec_ctx->channel_layout, -1};
    int out_sample_rates[] = {output_codec_ctx->sample_rate, -1};
    ret = av_opt_set_int_list(buffersink_ctx, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format");
        avfilter_graph_free(&graph);
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "channel_layouts", out_channel_layouts,-1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout");
        avfilter_graph_free(&graph);
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "sample_rates",out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate");
        avfilter_graph_free(&graph);
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "avfilter_graph_parse_ptr Error");
        avfilter_graph_free(&graph);
        goto end;
    }

    if ((ret = avfilter_graph_config(graph, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "avfilter_graph_config Error");
        avfilter_graph_free(&graph);
        goto end;
    }

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    *filter_graph = graph;
    return ret;
}


/**
 * @brief open_input_file
 * @param file_name
 * @param input_format_ctx
 * @param options
 * @return
 */
static int open_input_file(const char *file_name,
                           AVFormatContext **input_format_ctx, AVDictionary **options)
{
    av_log(NULL, AV_LOG_WARNING, "open_input_file file_name : %s", file_name);
    int error = 0;
    AVInputFormat *ifmt = av_find_input_format(INPUT_DEVICE_FORMAT_NAME);
    if (NULL == ifmt) {
        av_log(NULL, AV_LOG_ERROR, "can not find input format");
        return AVERROR_EXIT;
    }
    
    if ((error = avformat_open_input(input_format_ctx, INPUT_DEVICE_NAME, ifmt, options)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input file : %s error: %s", file_name, av_err2str(error));
        *input_format_ctx = NULL;
        return error;
    }

    /** Get information on the input file (number of streams etc.). */
    if ((error = avformat_find_stream_info(*input_format_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open find stream info error: %s", av_err2str(error));
        avformat_close_input(input_format_ctx);
        return error;
    }

    (*input_format_ctx)->interrupt_callback = interrupt_callback;
    return 0;
}

/**
 * @brief open_input_codec
 * @param input_format_ctx
 * @param input_codec_ctx
 * @param audio_stream_index
 * @return
 */
static int open_input_codec(AVFormatContext *input_format_ctx,
                            AVCodecContext **input_codec_ctx,
                            int *audio_stream_index)
{
    av_log(NULL, AV_LOG_WARNING, "open_input_codec");
    AVCodecContext *avctx = NULL;
    AVCodec *input_codec = NULL;
    int error = 0;
    int stream_index = -1;
    int i = 0;

    for (; i < input_format_ctx->nb_streams; i++) {
        if (AVMEDIA_TYPE_AUDIO == input_format_ctx->streams[i]->codecpar->codec_type) {
            stream_index = i;
            break;
        }
    }
    if (-1 == stream_index) {
        av_log(NULL, AV_LOG_ERROR, "Could not  find audio stream index");
        return AVERROR_EXIT;
    }
    av_log(NULL, AV_LOG_WARNING, "find audio stream index : %d", stream_index);
    if (!(input_codec = avcodec_find_decoder(input_format_ctx->streams[stream_index]->codecpar->codec_id))) {
        av_log(NULL, AV_LOG_ERROR, "Could not find input codec");
        return AVERROR_EXIT;
    }

    /** allocate a new decoding context */
    avctx = avcodec_alloc_context3(input_codec);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate a decoding context");
        return AVERROR(ENOMEM);
    }

    /** initialize the stream parameters with demuxer information */
    error = avcodec_parameters_to_context(avctx, input_format_ctx->streams[stream_index]->codecpar);
    if (error < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize avctx error: %s", av_err2str(error));
        avcodec_free_context(&avctx);
        return error;
    }

    /** Open the decoder for the audio stream to use it later. */
    if ((error = avcodec_open2(avctx, input_codec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR,  "Could not open input codec error: %s", av_err2str(error));
        avcodec_free_context(&avctx);
        return error;
    }

    /** Save the decoder context for easier access later. */
    *input_codec_ctx = avctx;
    *audio_stream_index = stream_index;
    return 0;
}

/**
 * @brief open_output_file
 * @param filename
 * @param format_name
 * @param output_format_context
 * @return
 */
static int open_output_file(const char *filename,
                            const char* format_name,
                            AVFormatContext **output_format_context)
{
    if (NULL == filename) {
        av_log(NULL, AV_LOG_ERROR, "open_output_file filename NULL Error");
        return AVERROR_EXIT;
    }
    if (NULL == format_name) {
        av_log(NULL, AV_LOG_WARNING, "open_output_file : filename : %s", filename);
    }
    else {
        av_log(NULL, AV_LOG_WARNING, "open_output_file : filename : %s format_name : %s", filename, format_name);
    }

    int error = 0;
    if ((error = avformat_alloc_output_context2(output_format_context, NULL, format_name, filename)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "avformat_alloc_output_context2 error: %s", av_err2str(error));
        *output_format_context = NULL;
        return error;
    }
    return 0;
}

/**
 * @brief open_output_codec
 * @param output_format_context
 * @param output_codec_context
 * @param codec_name
 * @param input_format_context
 * @param para
 * @return
 */
static int open_output_codec(AVFormatContext *output_format_context,
                             AVCodecContext **output_codec_context,
                             const char *codec_name,
                             AVFormatContext *input_format_context,
                             OutputCodecPara *para) {
    AVCodecContext *avctx          = NULL;
    AVStream *stream               = NULL;
    AVCodec *output_codec          = NULL;
    int error = 0;
    /** Find the encoder to be used by its name. */
    if(NULL != codec_name) {
        if (!(output_codec = avcodec_find_encoder_by_name(codec_name))) {
            av_log(NULL, AV_LOG_ERROR, "Could not find encoder : %s", codec_name);
            return AVERROR_EXIT;
        }
    }
    else {
        if (NULL == input_format_context) {
            av_log(NULL, AV_LOG_ERROR, "there is no encoder can be special");
            return AVERROR_EXIT;
        }
        else {
//            if (!(output_codec = avcodec_find_encoder(input_format_context->streams[audio_stream_index]->codecpar->codec_id))) {
//                av_log(NULL, AV_LOG_ERROR, "Could not find encoder : %s", codec_name);
//                return AVERROR_EXIT;
//            }
        }
    }

    /** Create a new audio stream in the output file container. */
    if (!(stream = avformat_new_stream(output_format_context, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "Could not create new stream");
        error = AVERROR(ENOMEM);
        return error;
    }

    avctx = avcodec_alloc_context3(output_codec);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate an encoding context");
        error = AVERROR(ENOMEM);
        return error;
    }

    /**
     * Set the basic encoder parameters.
     * The input file's sample rate is used to avoid a sample rate conversion.
     */

    avctx->channels = para->channels;
    avctx->channel_layout = para->channel_layout;
    avctx->sample_rate = para->sample_rate;
    avctx->sample_fmt = para->sample_fmt;
    avctx->bit_rate = 64000;
    /** Set the sample rate for the container. */

    stream->time_base.num = 1;
    stream->time_base.den = avctx->sample_rate;

    /**
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if ((output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    if ((error = avcodec_open2(avctx, output_codec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open output codec error : %s", av_err2str(error));
        avcodec_free_context(&avctx);
        return error;
    }

    if ((error = avcodec_parameters_from_context(stream->codecpar, avctx)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize stream parameters error: %s", av_err2str(error));
        avcodec_free_context(&avctx);
        return error;
    }

    /** Save the encoder context for easier access later. */
    *output_codec_context = avctx;
    outputStream = stream;

    return 0;
}

/**
 * @brief init_packet
 * @param packet
 */
static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    /** Set the packet data and size so that it is recognized as being empty. */
    packet->data = NULL;
    packet->size = 0;
}

/**
 * @brief init_input_frame
 * @param frame
 * @return
 */
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate input frame");
        return AVERROR(ENOMEM);
    }
    return 0;
}


/**
 * @brief init_fifo
 * @param fifo
 * @param output_codec_context
 * @return
 */
static int init_fifo(AVAudioFifo **fifo,
                     AVCodecContext *output_codec_context)
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate FIFO");
        return AVERROR(ENOMEM);
    }
    return 0;
}


/**
 * @brief write_output_file_header
 * @param output_format_context
 * @param option
 * @return
 */
static int write_output_file_header(AVFormatContext *output_format_context,
                                    AVDictionary **option)
{
    int error;
    if ((error = avformat_write_header(output_format_context, option)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not write output file header error : %s ", av_err2str(error));
        return error;
    }
    return 0;
}

/**
 * @brief decode_audio_frame
 * @param frame
 * @param input_format_context
 * @param input_codec_context
 * @param data_present
 * @param finished
 * @return
 */
static int decode_audio_frame(AVFrame *frame,
                              AVPacket *input_packet,
                              AVCodecContext *input_codec_context,
                              int *data_present,
                              int *finished)
{
    /** Packet used for temporary storage. */
    int error;
    /** Read one audio frame from the input file into a temporary packet. */

    /**
     * Decode the audio frame stored in the temporary packet.
     * The input audio stream decoder is used to do this.
     * If we are at the end of the file, pass an empty packet to the decoder
     * to flush it.
     */
    if ((error = avcodec_decode_audio4(input_codec_context, frame,
                                       data_present, input_packet)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not decode frame error : %s", av_err2str(error));
        return error;
    }

    /**
     * If the decoder has not been flushed completely, we are not finished,
     * so that this function has to be called again.
     */
    if (*finished && *data_present) {
        *finished = 0;
    }
    return 0;
}

/**
 * @brief add_samples_to_fifo
 * @param fifo
 * @param converted_input_samples
 * @param frame_size
 * @return
 */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error = 0;

    /**
     * Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples.
     */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        av_log(NULL, AV_LOG_ERROR,  "Could not reallocate FIFO error : %s", av_err2str(error));
        return error;
    }

    /** Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not write data to FIFO");
        return AVERROR_EXIT;
    }
    return 0;
}


/**
 * @brief read_decode_convert_and_store
 * @param fifo
 * @param input_format_context
 * @param input_codec_context
 * @param filter_graph
 * @param finished
 * @return
 */
static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVFilterGraph *filter_graph,
                                         int *finished, int audio_stream_index)
{
//    av_log(NULL, AV_LOG_WARNING, "read_decode_convert_and_store");
    /** Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    /** Temporary storage for the converted input samples. */
    int data_present;
    int ret = AVERROR_EXIT;
    AVPacket input_pkt;
    init_packet(&input_pkt);
    ret = av_read_frame(input_format_context, &input_pkt);
    if (ret != 0) {
        av_packet_unref(&input_pkt);
        if (ret == AVERROR_EOF) {
            *finished = 1;
            return 0;
        }
        else {
            av_log(NULL, AV_LOG_ERROR,  "Could not read frame error : %s", av_err2str(ret));
            return ret;
        }
    }

    if (input_pkt.stream_index != audio_stream_index) {
        av_log(NULL, AV_LOG_ERROR, "read_decode_convert_and_store ignore not audio packet");
        av_packet_unref(&input_pkt);
        return 0;
    }
    /** Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame)) {
        av_log(NULL, AV_LOG_ERROR, "init_input_frame Error");
        av_packet_unref(&input_pkt);
        return AVERROR_EXIT;
    }

    /** Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, &input_pkt,
                           input_codec_context, &data_present, finished)) {
        av_log(NULL, AV_LOG_ERROR, "decode_audio_frame Error");
        av_frame_free(&input_frame);
        av_packet_unref(&input_pkt);
        return AVERROR_EXIT;
    }
    av_packet_unref(&input_pkt);


    /**
     *
     * If we are at the end of the file and there are no more samples
     * in the decoder which are delayed, we are actually finished.
     * This must not be treated as an error.
     */
    if (*finished && !data_present) {
        ret = 0;
        av_frame_free(&input_frame);
        av_log(NULL, AV_LOG_WARNING, "there is no decoded data");
        return ret;
    }
    /** If there is decoded data, convert and store it */
    if (data_present) {
        input_frame->pts = av_frame_get_best_effort_timestamp(input_frame);
        if (NULL != filter_graph) {
            AVFilterContext *buffer_src_ctx = avfilter_graph_get_filter(filter_graph, FILTER_BUFFER_SRC_NAME);
            AVFilterContext *buffer_sink_ctx = avfilter_graph_get_filter(filter_graph, FILTER_BUFFER_SINK_NAME);
            if (NULL == buffer_src_ctx || NULL == buffer_sink_ctx) {
                av_log(NULL, AV_LOG_ERROR, "avfilter_graph_get_filter Error");
                av_frame_free(&input_frame);
                return AVERROR_EXIT;
            }


            /* push the audio data from decoded frame into the filtergraph */
//            av_log(NULL, AV_LOG_WARNING, "\n input_frame->format %d\n", input_frame->format);
            if (av_buffersrc_add_frame_flags(buffer_src_ctx, input_frame, 0) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph");
                av_frame_free(&input_frame);
                return AVERROR_EXIT;
            }

            av_frame_unref(input_frame);
            ret = av_buffersink_get_frame(buffer_sink_ctx, input_frame);
            if (ret >=0) {
//                 av_log(NULL, AV_LOG_WARNING, "\n input_frame->format %d\n", input_frame->format);
            }
            else if (AVERROR(EAGAIN) == ret || AVERROR_EOF == ret) {
                av_log(NULL, AV_LOG_WARNING, "av_buffersink_get_frame Need to feed more frames in");
                av_frame_free(&input_frame);
                return 0;
            }
            else {
                av_log(NULL, AV_LOG_ERROR, "av_buffersink_get_frame error: %s", av_err2str(ret));
                av_frame_free(&input_frame);
                return ret;
            }
        }
        /** Add the converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, input_frame->extended_data,
                                input_frame->nb_samples)) {
            av_log(NULL, AV_LOG_ERROR, "add_samples_to_fifo error");
            av_frame_free(&input_frame);
            return AVERROR_EXIT;
        }
    }
    av_frame_free(&input_frame);
    ret = 0;
    return ret;
}

/**
 * @brief init_output_frame
 * @param frame
 * @param output_codec_context
 * @param frame_size
 * @return
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error = 0;

    /** Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate output frame");
        return AVERROR_EXIT;
    }

    /**
     * Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity.
     */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /**
     * Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified.
     */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could allocate output frame samples error : %s", av_err2str(error));
        av_frame_free(frame);
        return error;
    }
    return 0;
}

/** Global timestamp for the audio frames */
static int64_t pts = 0;

/**
 * @brief encode_audio_frame
 * @param frame
 * @param output_format_context
 * @param output_codec_context
 * @param data_present
 * @return
 */
static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    int current_duration = av_rescale(pts, output_codec_context->time_base.num, output_codec_context->time_base.den);
    if ((0 != request_duration) && (current_duration > request_duration)) {
        av_log(NULL, AV_LOG_WARNING, "request_duration is %f current pts is %f exit!!!!!!", request_duration, current_duration);
        return AVERROR_EXIT;
    }
    /** Packet used for temporary storage. */
    AVPacket output_packet;
    int error = 0;
    init_packet(&output_packet);

    /** Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }

    /**
     * Encode the audio frame and store it in the temporary packet.
     * The output audio stream encoder is used to do this.
     */
    if ((error = avcodec_encode_audio2(output_codec_context, &output_packet,
                                       frame, data_present)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not encode frame error : %s", av_err2str(error));
        av_packet_unref(&output_packet);
        return error;
    }

    /** Write one audio frame from the temporary packet to the output file. */
    if (*data_present) {
        av_packet_rescale_ts(&output_packet, output_codec_context->time_base, outputStream->time_base);
        output_packet.stream_index = outputStream->index;
        error = av_write_frame(output_format_context, &output_packet);
        if (error < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not write frame (error : %s", av_err2str(error));
            av_packet_unref(&output_packet);
            return error;
        }
        else {
            log_packet(output_format_context, &output_packet);
        }
        av_packet_unref(&output_packet);
    }
    return 0;
}

/**
 * @brief load_encode_and_write
 * @param fifo
 * @param output_format_context
 * @param output_codec_context
 * @return
 */
static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    /** Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /**
     * Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame size
     */
    const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                 output_codec_context->frame_size);
    int data_written;

    /** Initialize temporary storage for one output frame. */
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
        return AVERROR_EXIT;

    /**
     * Read as many samples from the FIFO buffer as required to fill the frame.
     * The samples are stored in the frame temporarily.
     */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not read data from FIFO");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /** Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written)) {
        av_frame_free(&output_frame);
        if (request_duration > 0) {
            request_duration = 0;
        }
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

/**
 * @brief write_output_file_trailer
 * @param output_format_context
 * @return
 */
static int write_output_file_trailer(AVFormatContext *output_format_context)
{
    int error;
    if ((error = av_write_trailer(output_format_context)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not write output file trailer error : %s", av_err2str(error));
        return error;
    }
    return 0;
}

/**
 * @brief ffmpeg_init
 * @return
 */
int ffmpeg_init()
{
    av_register_all();
    avdevice_register_all();
    avcodec_register_all();
    avfilter_register_all();
    avformat_network_init();
    
    av_log_set_callback(custom_log_callback);
    av_log_set_level(global_log_level);
    request_duration = 0.0;
    abort_flag = 0;
    ts_duration = 0.0;
    tsComplateCB = NULL;
    tsIndex = 0;
    outputStream = NULL;
    signal(SIGTERM,signalHandler);
    return 0;
}


/**
 * @brief audio_converter
 * @param inputFile
 * @param outputFile
 * @return
 */
int audio_converter(const char *inputFile, const char *outputFile)
{
    av_log(NULL, AV_LOG_WARNING, "audio_converter inputFile : %s  outputFile : %s", inputFile, outputFile);
    if (0.0 - getTsDuration() != 0) {
        ts_duration = getTsDuration();
    }
    if (NULL != outputFile) {
        strncpy(outFile, outputFile, strlen(outputFile));
    }

    AVFormatContext *input_format_context = NULL;
    AVFormatContext *output_format_context = NULL;
    AVCodecContext *input_codec_context = NULL;
    AVCodecContext *output_codec_context = NULL;
    AVAudioFifo *fifo = NULL;
    AVFilterGraph *filter_graph = NULL;
    int audio_stream_index = -1;
    int ret = AVERROR_EXIT;

    /** Open the input file for reading. */
    if (ret = open_input_file(inputFile, &input_format_context, &input_format_options)) {
        av_log(NULL, AV_LOG_WARNING, "%s open_input_file error %d", inputFile, ret);
        goto cleanup;
    }

    if (ret = open_input_codec(input_format_context, &input_codec_context, &audio_stream_index)) {
        av_log(NULL, AV_LOG_WARNING, "%s open_input_codec error %d", inputFile, ret);
        goto cleanup;
    }


    input_codec_context->request_sample_fmt = global_para.sample_fmt;
    input_codec_context->request_channel_layout = global_para.channel_layout;
    input_codec_context->flags = CODEC_FLAG_LOW_DELAY;


    /** Open the output file for writing. */
    if (ret = open_output_file(outputFile, global_para.output_format_name, &output_format_context)) {
        av_log(NULL, AV_LOG_WARNING, "%s open_output_file error %d", inputFile, ret);
        goto cleanup;
    }

    if (ret = open_output_codec(output_format_context, &output_codec_context, global_para.codec_name, NULL, &global_para)) {
        av_log(NULL, AV_LOG_WARNING, "%s open_output_codec error %d", inputFile, ret);
        goto cleanup;
    }
    output_codec_context->flags = CODEC_FLAG_LOW_DELAY;

    /** Initialize the resampler to be able to convert audio sample formats. */
    if (ret = init_filters("anull", input_codec_context, output_codec_context, &filter_graph)) {
        av_log(NULL, AV_LOG_WARNING, "%s init_filters error %d", inputFile, ret);
        goto cleanup;
    }

    /** Initialize the FIFO buffer to store audio samples to be encoded. */
    if (ret = init_fifo(&fifo, output_codec_context)) {
        av_log(NULL, AV_LOG_WARNING, "%s init_fifo error %d", inputFile, ret);
        goto cleanup;
    }

    /** Write the header of the output file container. */
    if (ret = write_output_file_header(output_format_context, &output_format_options)) {
        av_log(NULL, AV_LOG_WARNING, "%s write_output_file_header error %d", inputFile, ret);
        goto cleanup;
    }

    /**
     * Loop as long as we have input samples to read or output samples
     * to write; abort as soon as we have neither.
     */
    while (1) {
        if (abort_flag) {
            av_log(NULL, AV_LOG_WARNING, "%s abort_flag request exit", inputFile);
            goto cleanup;
        }
        /** Use the encoder's desired frame size for processing. */

        const int output_frame_size = output_codec_context->frame_size;
        int finished                = 0;

        /**
         * Make sure that there is one frame worth of samples in the FIFO
         * buffer so that the encoder can do its work.
         * Since the decoder's and the encoder's frame size may differ, we
         * need to FIFO buffer to store as many frames worth of input samples
         * that they make up at least one frame worth of output samples.
         */
        while (av_audio_fifo_size(fifo) < output_frame_size)
        {
            if (abort_flag) {
                av_log(NULL, AV_LOG_WARNING, "%s abort_flag request2 exit", inputFile);
                goto cleanup;
            }
            /**
             * Decode one frame worth of audio samples, convert it to the
             * output sample format and put it into the FIFO buffer.
             */
            if (ret = read_decode_convert_and_store(fifo, input_format_context,
                                              input_codec_context,
                                              filter_graph,
                                              &finished, audio_stream_index)) {
                av_log(NULL, AV_LOG_WARNING, "%s read_decode_convert_and_store error %d", inputFile, ret);
                goto cleanup;
            }

            /**
             * If we are at the end of the input file, we continue
             * encoding the remaining audio samples to the output file.
             */
            if (finished) {
                break;
            }
        }

        /**
         * If we have enough samples for the encoder, we encode them.
         * At the end of the file, we pass the remaining samples to
         * the encoder.
         */
        while (av_audio_fifo_size(fifo) >= output_frame_size ||
               (finished && av_audio_fifo_size(fifo) > 0)) {
            if (abort_flag) {
                av_log(NULL, AV_LOG_WARNING, "%s abort_flag request3 exit", inputFile);
                goto cleanup;
            }
            /**
             * Take one frame worth of audio samples from the FIFO buffer,
             * encode it and write it to the output file.
             */
            if (ret = load_encode_and_write(fifo, output_format_context,
                                      output_codec_context)) {
                av_log(NULL, AV_LOG_WARNING, "%s load_encode_and_write error %d", inputFile, ret);
                goto cleanup;
            }
        }

        /**
         * If we are at the end of the input file and have encoded
         * all remaining samples, we can exit this loop and finish.
         */
        if (finished) {
            int data_written;
            /** Flush the encoder as it may have delayed frames. */
            do {
                if (abort_flag) {
                    av_log(NULL, AV_LOG_WARNING, "%s abort_flag request3 exit", inputFile);
                    goto cleanup;
                }
                if (ret = encode_audio_frame(NULL, output_format_context,
                                       output_codec_context, &data_written)) {
                    av_log(NULL, AV_LOG_WARNING, "%s encode_audio_frame error %d", inputFile, ret);
                    goto cleanup;
                }
            } while (data_written);
            break;
        }
    }

    /** Write the trailer of the output file container. */
    if (ret = write_output_file_trailer(output_format_context)) {
        av_log(NULL, AV_LOG_WARNING, "%s write_output_file_trailer error %d", inputFile, ret);
        goto cleanup;
    }
    char temp[LOG_BUFFER_LEN] = { 0 };
    snprintf(temp, LOG_BUFFER_LEN, outFile, tsIndex);
    if (NULL != tsComplateCB) {
        tsComplateCB(temp,1);
    }
    ret = 0;

cleanup:
    if (abort_flag) {
        ret = 0;
    }
    if (fifo) {
        av_audio_fifo_free(fifo);
    }

    if (output_codec_context) {
        avcodec_free_context(&output_codec_context);
    }

    if (output_format_context) {
        avformat_free_context(output_format_context);
    }

    if (input_codec_context) {
        avcodec_free_context(&input_codec_context);
    }

    if (input_format_context) {
        avformat_close_input(&input_format_context);
    }

    if (filter_graph) {
        avfilter_graph_free(&filter_graph);
    }

    if(input_format_options) {
        av_dict_free(&input_format_options);
    }

    if (input_codec_options) {
        av_dict_free(&input_codec_options);
    }

    if (output_codec_options) {
        av_dict_free(&output_codec_options);
    }

    if (output_format_options) {
        av_dict_free(&output_format_options);
    }
    return ret;
}

void print(const char* mess)
{
    printf("%s", mess);
    fflush(stdout);
}


int main()
{

    ffmpeg_init();
    set_global_log_level(32);
    set_global_log_print(print);
    char input[] = "test";
    char output[] = "./test_%d.ts";
    return audio_converter(input, output);


}

