/* Copyright (c) 2018 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdio.h>
#include "nnet_data.h"
#include "nnet.h"
#include "common.h"
#include "arch.h"
#include "lpcnet.h"
#include "lpcnet_private.h"

#define PREEMPH 0.85f

#define PITCH_GAIN_FEATURE 37
#define PDF_FLOOR 0.002

#define FRAME_INPUT_SIZE (NB_FEATURES + EMBED_PITCH_OUT_SIZE)


#if 0
static void print_vector(float *x, int N)
{
    int i;
    for (i=0;i<N;i++) printf("%f ", x[i]);
    printf("\n");
}
#endif

void run_frame_network(LPCNetState *lpcnet, float *condition, float *gru_a_condition, const float *features, int pitch)
{
    NNetState *net;
    float in[FRAME_INPUT_SIZE];
    float conv1_out[FEATURE_CONV1_OUT_SIZE];
    float conv2_out[FEATURE_CONV2_OUT_SIZE];
    float dense1_out[FEATURE_DENSE1_OUT_SIZE];
    net = &lpcnet->nnet;
    RNN_COPY(in, features, NB_FEATURES);

    compute_embedding(&embed_pitch, &in[NB_FEATURES], pitch);
    compute_conv1d(&feature_conv1, conv1_out, net->feature_conv1_state, in);
    if (lpcnet->frame_count < FEATURE_CONV1_DELAY) RNN_CLEAR(conv1_out, FEATURE_CONV1_OUT_SIZE);
    compute_conv1d(&feature_conv2, conv2_out, net->feature_conv2_state, conv1_out);
    celt_assert(FRAME_INPUT_SIZE == FEATURE_CONV2_OUT_SIZE);
    if (lpcnet->frame_count < FEATURES_DELAY) RNN_CLEAR(conv2_out, FEATURE_CONV2_OUT_SIZE);
    memmove(lpcnet->old_input[1], lpcnet->old_input[0], (FEATURES_DELAY-1)*FRAME_INPUT_SIZE*sizeof(in[0]));
    memcpy(lpcnet->old_input[0], in, FRAME_INPUT_SIZE*sizeof(in[0]));
    compute_dense(&feature_dense1, dense1_out, conv2_out);
    compute_dense(&feature_dense2, condition, dense1_out);
    compute_dense(&gru_a_dense_feature, gru_a_condition, condition);
    if (lpcnet->frame_count < 1000) lpcnet->frame_count++;

}

void run_sample_network(NNetState *net, int *exc, const float *condition, const float *gru_a_condition, int *last_exc, int *last_sig, int *pred, float pitch_gain)
{
    float gru_a_input[3*GRU_A_STATE_SIZE];
    float in_b[GRU_A_STATE_SIZE+FEATURE_DENSE2_OUT_SIZE];
    float pdf_1[DUAL_FC_1_OUT_SIZE];
    float pdf_2[DUAL_FC_2_OUT_SIZE];
    float exc_embed_out[MAX_MDENSE_TMP];

    RNN_COPY(gru_a_input, gru_a_condition, 3*GRU_A_STATE_SIZE);
    accum_embedding(&gru_a_embed_sig_1, gru_a_input, last_sig[1]);
    accum_embedding(&gru_a_embed_pred_1, gru_a_input, pred[1]);
    accum_embedding(&gru_a_embed_exc_1, gru_a_input, last_exc[1]);

    accum_embedding(&gru_a_embed_sig_0, gru_a_input, last_sig[0]);
    accum_embedding(&gru_a_embed_pred_0, gru_a_input, pred[0]);
    accum_embedding(&gru_a_embed_exc_0, gru_a_input, last_exc[0]);

    compute_sparse_gru(&sparse_gru_a, net->gru_a_state, gru_a_input);
    RNN_COPY(in_b, net->gru_a_state, GRU_A_STATE_SIZE);
    RNN_COPY(&in_b[GRU_A_STATE_SIZE], condition, FEATURE_DENSE2_OUT_SIZE);
    compute_gru2(&gru_b, net->gru_b_state, in_b);
    compute_mdense(&dual_fc_1, pdf_1, net->gru_b_state);

    /* 采样第一个激励信号 */
    exc[1] = sample_from_pdf(pdf_1, DUAL_FC_1_OUT_SIZE, MAX16(0, 1.5f*pitch_gain - .5f), PDF_FLOOR);
    /*
    // 计算第一个激励信号的embedding，存储在context_1的GRU_B_STATE_SIZE后面，因为从0~GRU_B_STATE_SIZE要存储gru_b_state
    compute_embedding(&embed_sig, &context_1[GRU_B_STATE_SIZE], exc[1]);
    // 复制gru_b_state到context_1的0~GRU_B_STATE_SIZE位置
    RNN_COPY(context_1, net->gru_b_state, GRU_B_STATE_SIZE);
    / 从新的context计算第二个激励信号
    compute_mdense(&dual_fc_2, pdf_2, context_1);
    */
    compute_embedding(&md_embed_sig, exc_embed_out, exc[1]);
    compute_mdense2(&dual_fc_2, pdf_2, net->gru_b_state, exc_embed_out);
    exc[0] = sample_from_pdf(pdf_2, DUAL_FC_2_OUT_SIZE, MAX16(0, 1.5f*pitch_gain - .5f), PDF_FLOOR);
}

LPCNET_EXPORT int lpcnet_get_size()
{
    return sizeof(LPCNetState);
}

LPCNET_EXPORT int lpcnet_init(LPCNetState *lpcnet)
{
    memset(lpcnet, 0, lpcnet_get_size());
    lpcnet->last_exc[0] = lin2ulaw(0.f);
    lpcnet->last_exc[1] = lin2ulaw(0.f);
    return 0;
}


LPCNET_EXPORT LPCNetState *lpcnet_create()
{
    LPCNetState *lpcnet;
    lpcnet = (LPCNetState *)calloc(lpcnet_get_size(), 1);
    lpcnet_init(lpcnet);
    return lpcnet;
}

LPCNET_EXPORT void lpcnet_destroy(LPCNetState *lpcnet)
{
    free(lpcnet);
}

LPCNET_EXPORT void lpcnet_synthesize(LPCNetState *lpcnet, const float *features, short *output, int N)
{
    int i;
    float condition[FEATURE_DENSE2_OUT_SIZE];
    float lpc[LPC_ORDER];
    float gru_a_condition[3*GRU_A_STATE_SIZE];
    int pitch;
    float pitch_gain;
    /* Matches the Python code -- the 0.1 avoids rounding issues. */
    pitch = (int)floor(.1 + 50*features[36]+100);
    pitch = IMIN(255, IMAX(33, pitch));
    pitch_gain = lpcnet->old_gain[FEATURES_DELAY-1];
    memmove(&lpcnet->old_gain[1], &lpcnet->old_gain[0], (FEATURES_DELAY-1)*sizeof(lpcnet->old_gain[0]));
    lpcnet->old_gain[0] = features[PITCH_GAIN_FEATURE];
    run_frame_network(lpcnet, condition, gru_a_condition, features, pitch);
    memcpy(lpc, lpcnet->old_lpc[FEATURES_DELAY-1], LPC_ORDER*sizeof(lpc[0]));
    memmove(lpcnet->old_lpc[1], lpcnet->old_lpc[0], (FEATURES_DELAY-1)*LPC_ORDER*sizeof(lpc[0]));
    lpc_from_cepstrum(lpcnet->old_lpc[0], features);
    if (lpcnet->frame_count <= FEATURES_DELAY)
    {
        RNN_CLEAR(output, N);
        return;
    }
    for (i=0;i<N;i+=R)
    {
        int j;
        float pcm = 0.0;
        int exc[2] = {0, 0};
        int last_sig_ulaw[2];
        int pred_ulaw[2];
        float pred[2] = {0.0, 0.0};
        float pred_new = 0.0;  /* R-1个 */
        for (j=0;j<LPC_ORDER;j++) {
            pred[0] -= lpcnet->last_sig[j]*lpc[j];
            pred[1] -= lpcnet->last_sig[j+1]*lpc[j];
        }
        last_sig_ulaw[0] = lin2ulaw(lpcnet->last_sig[0]);
        last_sig_ulaw[1] = lin2ulaw(lpcnet->last_sig[1]);
        pred_ulaw[0] = lin2ulaw(pred[0]);
        pred_ulaw[1] = lin2ulaw(pred[1]);
        run_sample_network(&lpcnet->nnet, exc, condition, gru_a_condition, lpcnet->last_exc, last_sig_ulaw, pred_ulaw, pitch_gain);

        pcm = pred[0] + ulaw2lin(exc[1]);
        RNN_MOVE(&lpcnet->last_sig[1], &lpcnet->last_sig[0], LPC_ORDER);
        lpcnet->last_exc[1] = exc[1];
        lpcnet->last_sig[0] = pcm;
        pcm += PREEMPH*lpcnet->deemph_mem;
        lpcnet->deemph_mem = pcm;
        if (pcm<-32767) pcm = -32767;
        if (pcm>32767) pcm = 32767;
        output[i] = (int)floor(.5 + pcm);

        /* 根据新的样点计算新的pred */
        for (j=0;j<LPC_ORDER;j++) {
            pred_new -= lpcnet->last_sig[j]*lpc[j];
        }
        pcm = pred_new + ulaw2lin(exc[0]);
        RNN_MOVE(&lpcnet->last_sig[1], &lpcnet->last_sig[0], LPC_ORDER);
        lpcnet->last_exc[0] = exc[0];
        lpcnet->last_sig[0] = pcm;
        pcm += PREEMPH*lpcnet->deemph_mem;
        lpcnet->deemph_mem = pcm;
        if (pcm<-32767) pcm = -32767;
        if (pcm>32767) pcm = 32767;
        output[i+1] = (int)floor(.5 + pcm);
    }
}


LPCNET_EXPORT int lpcnet_decoder_get_size()
{
  return sizeof(LPCNetDecState);
}

LPCNET_EXPORT int lpcnet_decoder_init(LPCNetDecState *st)
{
  memset(st, 0, lpcnet_decoder_get_size());
  lpcnet_init(&st->lpcnet_state);
  return 0;
}

LPCNET_EXPORT LPCNetDecState *lpcnet_decoder_create()
{
  LPCNetDecState *st;
  st = malloc(lpcnet_decoder_get_size());
  lpcnet_decoder_init(st);
  return st;
}

LPCNET_EXPORT void lpcnet_decoder_destroy(LPCNetDecState *st)
{
  free(st);
}

LPCNET_EXPORT int lpcnet_decode(LPCNetDecState *st, const unsigned char *buf, short *pcm)
{
  int k;
  float features[4][NB_TOTAL_FEATURES];
  decode_packet(features, st->vq_mem, buf);
  for (k=0;k<4;k++) {
    lpcnet_synthesize(&st->lpcnet_state, features[k], &pcm[k*FRAME_SIZE], FRAME_SIZE);
  }
  return 0;
}

