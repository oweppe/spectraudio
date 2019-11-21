#include <stdio.h>
#include <SDL/SDL.h>
#include "kiss_fftr.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h>

#define WIDTH 1920
#define HEIGHT 1080
#define BPP 4
#define DEPTH 32

#define FAKE_FMIN 50
#define FAKE_FMAX 20000
#define FSAMP 22050

#define UVAL8(S16) ((S16/256)+128)
#define BUF_SIZE 400000
#define COPY_TRIG (BUF_SIZE - FSAMP)
#define FFT_SIZE 2048
#define FFT_SHIFT 256
#define INIT_GAIN 45
#define INIT_OFFSET 0

static char *device = "default";

typedef struct {
    Uint8 gain;
    Uint8 offset;
} fftcontext;

Uint8 logmag(kiss_fft_cpx value, fftcontext params)
{
    float x = (float)value.r;
    float y = (float)value.i;
    float d = x*x + y*y;
    float logd;
    if (d>0) logd = log(d/2.);
    else logd = -21;
    float res = logd*params.gain + params.offset;
    if (res > 255.) return 255;
    if (res > 0.) return (Uint8)res;
    return 0;
}

void hann(float *vectorin, float* hannwin, float* vectorout)
{
    for(int i=0;i < FFT_SIZE;i++) {
        vectorout[i]=vectorin[i]/2.*hannwin[i];
    }
}
Uint32 genaudio(float *buffer)
{
    Uint32 last_cnt = SDL_GetTicks();
    // Current frequency
    static Uint16 freq = FAKE_FMIN;
    // Remainging samples
    static Uint32 remaining = FSAMP/FAKE_FMIN;

    Uint32 current_cnt = SDL_GetTicks();
    Uint32 duration_ms;
    if (current_cnt - last_cnt < (1<<31) ) {
        duration_ms = current_cnt - last_cnt;
    }
    else {
        duration_ms = (~0)  - (current_cnt - last_cnt) + 1;
    }
    last_cnt = current_cnt;
    // Determine number of samples to generate
    Uint32 nsamples = duration_ms * FSAMP/1000;

    Uint32 index = 0;
    for(Uint32 index = 0; index < nsamples; index++)
    {
        if (remaining < (FSAMP / freq) / 2) {
            buffer[index]  = 0.9;
        }
        else {
            buffer[index]  = -0.9;
        }
        remaining--;
        if (remaining == 0) {
            freq++;
            if (freq == FAKE_FMAX) {
                freq = FAKE_FMIN;
            }
            remaining = FSAMP / freq;
        }
    }
    return nsamples;
}

void setpixel(SDL_Surface *screen, int x, int y, Uint8 r, Uint8 g, Uint8 b)
{
    Uint32 *pixmem32;
    Uint32 colour;

    colour = SDL_MapRGB( screen->format, r, g, b );

    pixmem32 = (Uint32*) screen->pixels  + y*screen->w + x;
    *pixmem32 = colour;
}

void DrawScreen(SDL_Surface* screen, kiss_fft_cpx* column, Uint16* logmap, fftcontext params)
{
    static int x = 0;
    int y, ytimesw;

    if(SDL_MUSTLOCK(screen)) {
        if(SDL_LockSurface(screen) < 0) return;
    }

    //for(y = 0; y < FFT_SIZE/2; y++ ) {
    //    ytimesw = (FFT_SIZE/2 - y);
    //    setpixel(screen, x, ytimesw, logmag(column[y], params), 0, 0);
    //}
    for(y = 0; y < screen->h; y++ ) {
        setpixel(screen, x, y, logmag(column[FFT_SIZE/2 - 1 - logmap[y]], params), 0, 0);
    }
    if (x< screen->w - 1) {
        x++;
    }
    else {
        x = 0;
    }

    if(SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);

    SDL_Flip(screen);
}


int main(int argc, char* argv[])
{

    int quit = 0;
    int h=0; 
    // SDL Initialization
    SDL_Surface *screen;
    SDL_Event event;
    if (SDL_Init(SDL_INIT_VIDEO) < 0 ) return 1;
    if (!(screen = SDL_SetVideoMode(WIDTH, HEIGHT, DEPTH, SDL_FULLSCREEN|SDL_HWSURFACE)))
    {
        SDL_Quit();
        return 1;
    }

    // ALSA initialization
    /* Open PCM device for recording (capture). */
    int dir = 0;
    char *alsabuffer;
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *alsaparams;
    int rc = snd_pcm_open(&handle, device, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        fprintf(stderr, "unable to open pcm device: %s\n", snd_strerror(rc));
        exit(1);
    }
    /* Allocate a hardware parameters object. */
    snd_pcm_hw_params_alloca(&alsaparams);
    /* Fill it in with default values. */
    snd_pcm_hw_params_any(handle, alsaparams);
    /* Set the desired hardware parameters. */
    /* Interleaved mode */
    snd_pcm_hw_params_set_access(handle, alsaparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    /* Signed 16-bit little-endian format */
    snd_pcm_hw_params_set_format(handle, alsaparams, SND_PCM_FORMAT_S16_LE);
    /* Two channels (stereo) */
    snd_pcm_hw_params_set_channels(handle, alsaparams, 2);
    /* 44100 bits/second sampling rate (CD quality) */
    unsigned int val = FSAMP;
    snd_pcm_hw_params_set_rate_near(handle, alsaparams, &val, &dir);
    /* Set period size to NFFT frames. */
    snd_pcm_uframes_t alsaframes = FFT_SHIFT;
    snd_pcm_hw_params_set_period_size_near(handle, alsaparams, &alsaframes, &dir);
    /* Write the parameters to the driver */
    rc = snd_pcm_hw_params(handle, alsaparams);
    if (rc < 0) {
        fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(rc));
        exit(1);
    }
    /* Use a buffer large enough to hold one period */
    snd_pcm_hw_params_get_period_size(alsaparams, &alsaframes, &dir);
    int alsasize = alsaframes * 4; /* 2 bytes/sample, 2 channels */
    alsabuffer = (char *) malloc(alsasize);

    // FFT Initialization
    fftcontext params;
    params.gain = INIT_GAIN;
    params.offset = INIT_OFFSET;
    float *samples_buf;
    float *han_buf;
    kiss_fft_cpx *fft_buf;
    Uint32 wrcnt = 0;
    Uint32 rdcnt = 0;
    samples_buf = (float *)malloc(BUF_SIZE*sizeof(kiss_fft_scalar));
    han_buf = (float *)malloc(FFT_SIZE*sizeof(float));
    fft_buf = (kiss_fft_cpx *)malloc(FFT_SIZE*sizeof(kiss_fft_cpx));
    kiss_fftr_cfg mycfg=kiss_fftr_alloc(FFT_SIZE,0,NULL,NULL);
    // preprocess hanning window
    float hannwin[FFT_SIZE];
    for(int i=0;i < FFT_SIZE;i++) {
        hannwin[i]=1.-cos(2.*M_PI*i/(FFT_SIZE-1));
    }
    // preprocess vertical log mapping
    Uint16 *logmap;
    logmap = (Uint16 *)malloc(screen->h*sizeof(Uint16));
    const float clogfact = (FFT_SIZE/2 - 1)/log(screen->h);
    logmap[0] = 0;
    for(int i=1;i <screen->h;i++) {
        logmap[i] = (Uint16)(clogfact*log(i));
    }
    while(!quit) {
        rc = snd_pcm_readi(handle, alsabuffer, alsaframes);
        if (rc == -EPIPE) {
            /* EPIPE means overrun */
            fprintf(stderr, "overrun occurred\n");
            snd_pcm_prepare(handle);
        } else if (rc < 0) {
            fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
        } else if (rc != (int)alsaframes) {
            fprintf(stderr, "short read, read %d alsaframes\n", rc);
        }
        // recopy to float
        for (int i=0;i<alsasize/2;i++) {
            Sint16* sampbuffer = (Sint16 *)alsabuffer;
            samples_buf[wrcnt + i] = sampbuffer[2*i]/65536. + sampbuffer[2*i+1]/65536.;
        }
         wrcnt += alsaframes;
         //wrcnt += genaudio(&samples_buf[wrcnt]);
         // manage buffer
         if (wrcnt > COPY_TRIG) {
             if(wrcnt - rdcnt > BUF_SIZE/2) {
                 printf("Abort, overflow\n");
                 return 0;
             }
             printf("recopy %d %d ", wrcnt, rdcnt);
             memcpy((Uint8 *)samples_buf, (Uint8 *)&samples_buf[rdcnt], sizeof(float)* (wrcnt - rdcnt));
             wrcnt = wrcnt - rdcnt;
             rdcnt = 0;
             printf("to %d %d \n", wrcnt, rdcnt);
         }
         if (wrcnt - rdcnt >= FFT_SIZE) {
            // perform FFT
            hann(&samples_buf[rdcnt], &hannwin[0], han_buf);
            kiss_fftr(mycfg, han_buf, fft_buf);
            DrawScreen(screen, fft_buf, logmap, params);
            rdcnt += FFT_SHIFT;
         }

         while(SDL_PollEvent(&event)) {
              switch (event.type) {
                  case SDL_QUIT:
	              quit = 1;
	              break;
                  case SDL_KEYDOWN: {
                      const Uint8 *state = SDL_GetKeyState(NULL);
                      if (state[SDLK_DOWN]) params.gain -= 1;
                      if (state[SDLK_UP]) params.gain += 1;
                      if (state[SDLK_RIGHT]) params.offset += 1;
                      if (state[SDLK_LEFT]) params.offset -= 1;
                      if (state[SDLK_ESCAPE]) quit = 1;
                      printf("gain=%d, offset=%d\n",params.gain, params.offset);
                      break;
                  }
              }
         }
    }

    SDL_Quit();
  
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(logmap);
    free(alsabuffer);
    free(samples_buf);
    free(han_buf);
    free(fft_buf);
    return 0;
}
