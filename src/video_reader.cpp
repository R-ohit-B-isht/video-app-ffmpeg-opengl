#include "video_reader.hpp"

// av_err2str returns a temporary array. This doesn't work in gcc.
// This function can be used as a replacement for av_err2str.
static const char* av_make_error(int errnum) {
    static char str[AV_ERROR_MAX_STRING_SIZE];
    memset(str, 0, sizeof(str));
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

static AVPixelFormat correct_for_deprecated_pixel_format(AVPixelFormat pix_fmt) {
    // Fix swscaler deprecated pixel format warning
    // (YUVJ has been deprecated, change pixel format to regular YUV)
    switch (pix_fmt) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default:                  return pix_fmt;
    }
}

bool video_reader_open(VideoReaderState* state, const char* filename) {

    // Unpack members of state
    auto& width = state->width;
    auto& height = state->height;
    auto& time_base = state->time_base;
    auto& av_format_ctx = state->av_format_ctx;
    auto& av_codec_ctx = state->av_codec_ctx;
    auto& video_stream_index = state->video_stream_index;
    auto& av_frame = state->av_frame;
    auto& av_packet = state->av_packet;

    // Open the file using libavformat
    av_format_ctx = avformat_alloc_context();
    if (!av_format_ctx) {
        printf("Couldn't created AVFormatContext\n");
        return false;
    }

    if (avformat_open_input(&av_format_ctx, filename, NULL, NULL) != 0) {
        printf("Couldn't open video file\n");
        return false;
    }

    // Find the first valid video stream inside the file
    video_stream_index = -1;
    AVCodecParameters* av_codec_params;
    AVCodec* av_codec;
    for (int i = 0; i < av_format_ctx->nb_streams; ++i) {
        av_codec_params = av_format_ctx->streams[i]->codecpar;
        av_codec = const_cast<AVCodec*>(avcodec_find_decoder(av_codec_params->codec_id));
        if (!av_codec) {
            continue;
        }
        if (av_codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            width = av_codec_params->width;
            height = av_codec_params->height;
            time_base = av_format_ctx->streams[i]->time_base;
            break;
        }
    }
    if (video_stream_index == -1) {
        printf("Couldn't find valid video stream inside file\n");
        return false;
    }

    // Set up a codec context for the decoder
    av_codec_ctx = avcodec_alloc_context3(av_codec);
    if (!av_codec_ctx) {
        printf("Couldn't create AVCodecContext\n");
        return false;
    }
    if (avcodec_parameters_to_context(av_codec_ctx, av_codec_params) < 0) {
        printf("Couldn't initialize AVCodecContext\n");
        return false;
    }
    if (avcodec_open2(av_codec_ctx, av_codec, NULL) < 0) {
        printf("Couldn't open codec\n");
        return false;
    }

    av_frame = av_frame_alloc();
    if (!av_frame) {
        printf("Couldn't allocate AVFrame\n");
        return false;
    }
    av_packet = av_packet_alloc();
    if (!av_packet) {
        printf("Couldn't allocate AVPacket\n");
        return false;
    }

    return true;
}

bool video_reader_read_frame(VideoReaderState* state, uint8_t* frame_buffer, int64_t* pts) {

    // Unpack members of state
    auto& width = state->width;
    auto& height = state->height;
    auto& av_format_ctx = state->av_format_ctx;
    auto& av_codec_ctx = state->av_codec_ctx;
    auto& video_stream_index = state->video_stream_index;
    auto& av_frame = state->av_frame;
    auto& av_packet = state->av_packet;
    // auto& sws_scaler_ctx = state->sws_scaler_ctx;
    auto& av_filter_graph = state->av_filter_graph;

    // Decode one frame
    int response;
    while (av_read_frame(av_format_ctx, av_packet) >= 0) {
        if (av_packet->stream_index != video_stream_index) {
            av_packet_unref(av_packet);
            continue;
        }

        response = avcodec_send_packet(av_codec_ctx, av_packet);
        if (response < 0) {
            printf("Failed to decode packet: %s\n", av_make_error(response));
            return false;
        }

        response = avcodec_receive_frame(av_codec_ctx, av_frame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
            av_packet_unref(av_packet);
            continue;
        } else if (response < 0) {
            printf("Failed to decode packet: %s\n", av_make_error(response));
            return false;
        }

        av_packet_unref(av_packet);
        break;
    }

    *pts = av_frame->pts;
    
    // Set up sws scaler
    // if (!sws_scaler_ctx) {
    //     auto source_pix_fmt = correct_for_deprecated_pixel_format(av_codec_ctx->pix_fmt);
    //     sws_scaler_ctx = sws_getContext(width, height, source_pix_fmt,
    //                                     width, height, AV_PIX_FMT_RGB0,
    //                                     SWS_BILINEAR, NULL, NULL, NULL);
    // }
    // if (!sws_scaler_ctx) {
    //     printf("Couldn't initialize sw scaler\n");
    //     return false;
    // }

    uint8_t* dest[4] = { frame_buffer, NULL, NULL, NULL };
    int dest_linesize[4] = { width * 4, 0, 0, 0 };
    // sws_scale(sws_scaler_ctx, av_frame->data, av_frame->linesize, 0, av_frame->height, dest, dest_linesize);


// Set up the filter graph
    if (!av_filter_graph) {
        av_filter_graph = avfilter_graph_alloc();
        if (!av_filter_graph) {
            printf("Couldn't allocate AVFilterGraph\n");
            return false;
        }
    } 
    
    // Create the buffersrc filter
    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    AVFilterContext* buffersrc_ctx;
    response = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", NULL, NULL, av_filter_graph);
    if (response < 0) {
        printf("Failed to create buffer source filter: %s\n", av_make_error(response));
        return false;
    }

    char* args = nullptr;
    int args_size = snprintf(NULL, 0, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        av_frame->width, av_frame->height, av_frame->format,
        av_frame->time_base.num, av_frame->time_base.den,
        av_frame->sample_aspect_ratio.num, av_frame->sample_aspect_ratio.den);
    args = new char[args_size + 1]; // Allocate memory for args

    snprintf(args, args_size + 1, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        av_frame->width, av_frame->height, av_frame->format,
        av_frame->time_base.num, av_frame->time_base.den,
        av_frame->sample_aspect_ratio.num, av_frame->sample_aspect_ratio.den);

    response = avfilter_init_str(buffersrc_ctx, args);
    av_freep(&args); 
    if (response < 0) {
        printf("Failed to initialize buffer source filter: %s\n", av_make_error(response));
        return false;
    }
    
    // Create the buffersink filter
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    AVFilterContext* buffersink_ctx;
    response = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, av_filter_graph);
    if (response < 0) {
        printf("Failed to create buffer sink filter: %s\n", av_make_error(response));
        return false;
    }
    
    // Set the parameters for the buffersink filter
    AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGB0, AV_PIX_FMT_NONE};
    response = av_opt_set_int_list(buffersink_ctx, "pix_fmts", (const int64_t*)pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (response < 0) {
        printf("Failed to set pixel formats for buffer sink filter: %s\n", av_make_error(response));
        return false;
    }
    
    // Link the filters
    response = avfilter_link(buffersrc_ctx, 0, buffersink_ctx, 0);
    if (response < 0) {
        printf("Failed to link filters: %s\n", av_make_error(response));
        return false;
    }
    
    response = avfilter_graph_config(av_filter_graph, NULL);
    if (response < 0) {
        printf("Failed to configure filter graph: %s\n", av_make_error(response));
        return false;
    }
    
    // Convert the frame using the filter graph
    AVFrame* filtered_frame = av_frame_alloc();
    if (!filtered_frame) {
        printf("Couldn't allocate AVFrame for filtered frame\n");
        return false;
    }
    response = av_buffersrc_add_frame_flags(buffersrc_ctx, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (response < 0) {
        printf("Failed to add frame to buffer source: %s\n", av_make_error(response));
        av_frame_free(&filtered_frame);
        return false;
    }
    response = av_buffersink_get_frame(buffersink_ctx, filtered_frame);
    if (response < 0) {
        printf("Failed to get filtered frame from buffer sink: %s\n", av_make_error(response));
        av_frame_free(&filtered_frame);
        return false;
    }
    
    // Copy the filtered frame to the output buffer
    // int dest_linesize[4] = { width * 4, 0, 0, 0 };
    av_image_copy_plane(frame_buffer, dest_linesize[0], filtered_frame->data[0], filtered_frame->linesize[0], width * 4, height);
    
    av_frame_free(&filtered_frame);


    return true;
}

bool video_reader_seek_frame(VideoReaderState* state, int64_t ts) {
    
    // Unpack members of state
    auto& av_format_ctx = state->av_format_ctx;
    auto& av_codec_ctx = state->av_codec_ctx;
    auto& video_stream_index = state->video_stream_index;
    auto& av_packet = state->av_packet;
    auto& av_frame = state->av_frame;
    
    av_seek_frame(av_format_ctx, video_stream_index, ts, AVSEEK_FLAG_BACKWARD);

    // av_seek_frame takes effect after one frame, so I'm decoding one here
    // so that the next call to video_reader_read_frame() will give the correct
    // frame
    int response;
    while (av_read_frame(av_format_ctx, av_packet) >= 0) {
        if (av_packet->stream_index != video_stream_index) {
            av_packet_unref(av_packet);
            continue;
        }

        response = avcodec_send_packet(av_codec_ctx, av_packet);
        if (response < 0) {
            printf("Failed to decode packet: %s\n", av_make_error(response));
            return false;
        }

        response = avcodec_receive_frame(av_codec_ctx, av_frame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
            av_packet_unref(av_packet);
            continue;
        } else if (response < 0) {
            printf("Failed to decode packet: %s\n", av_make_error(response));
            return false;
        }

        av_packet_unref(av_packet);
        break;
    }

    return true;
}

void video_reader_close(VideoReaderState* state) {
    if (state->av_filter_graph) {
        avfilter_graph_free(&state->av_filter_graph);
        state->av_filter_graph = nullptr;
    }
    // sws_freeContext(state->sws_scaler_ctx);
    avformat_close_input(&state->av_format_ctx);
    avformat_free_context(state->av_format_ctx);
    av_frame_free(&state->av_frame);
    av_packet_free(&state->av_packet);
    avcodec_free_context(&state->av_codec_ctx);
}
