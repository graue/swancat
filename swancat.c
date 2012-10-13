#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <err.h>
#include "xm.h"
#include "fastsin.h"
#include "crandom.h"
#include "rate.inc"

typedef struct
{
	FILE *fp;
	int maxsamps; // the longest the sound source is allowed to go
	int replacesamps; // point at which to replace it, if still going
	int replaced; // has already had a replacement made? 1=yes, 0=no
	int sampcount; // samples produced so far
} sndsrc_t;

static sndsrc_t **sndsrcs;
static int ssndsrcs = 0, nsndsrcs = 0;

#undef MIN
#undef MAX
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#define M_20_OVER_LN10 8.68588963806503655302257838
#define M_LN10_OVER_20 0.115129254649702284200899573
#define RATTODB(x) (log(x) * M_20_OVER_LN10)
#define DBTORAT(x) exp((x) * M_LN10_OVER_20)

#define OCTAVES 5
#define SEMIRANGE (OCTAVES*12) // span this many semitones
#define CENTERNOTE 440.0 // A-4 freq

#define SUBDIV 32 // subbeat = 1/32nd or 2^-5 of a beat
#define SUBPOT 5 // subdiv power of two

static int lastnote = SEMIRANGE/2+3; // 3 above A-4 (440 Hz) = C-5, I guess

static float beatlength; // in secs
static int minbeats, maxbeats;

static const char *fadetypes[] = {"lin", "log", "cos", "logcos"};

// which notes are included in the scale used
// 0 = note not included, 1 = note included
//was                      A   B C   D   E F   G     = A minor / C major?
// static int scale[12] = {1,0,1,1,0,1,0,1,1,0,1,0};

static int scale[12] = {1,0,1,1,0,0,0,1,1,0,0,0}; // B iwato (BCEFAB)

// starting with a note value, increment or descend by this many steps
// using only notes in the scale
static int stepsfrom(int startnote, int steps)
{
	int notesinscale = 0, ix;
	int newnote = startnote;

	if (!scale[startnote % 12])
		warnx("stepsfrom: startnote %d not in scale", startnote);

	for (ix = 0; ix < 12; ix++)
	{
		if (scale[ix])
			notesinscale++;
	}

	while (steps < 0)
	{
		newnote -= 12;
		steps += notesinscale;
	}

	while (steps > 0)
	{
		do {
			newnote++;
		} while (!scale[newnote % 12]);
		steps--;
	}

	return newnote;
}

static void install_gen(char *cmd, const size_t buflen,
	const float freq, const float len, const float amp)
{
	int which;

	which = rnd(6);
	if (which == 0) // sine or tri
	{
		snprintf(cmd, buflen, "%s -freq %f -len %f -amp %f",
			rnd(2) ? "sine" : "tri",
			freq, len, amp);
	}
	else if (which == 1) // square or octagon
	{
		snprintf(cmd, buflen, "%s -freq %f -len %f -amp %f",
			rnd(2) ? "square" : "octagon",
			freq, len, amp);
	}
	else if (which == 2) // saw
	{
		// 50-50 of being saw up or saw down
		snprintf(cmd, buflen, "saw %s -freq %f -len %f -amp %f",
			rnd(2) ? "-down" : "",
			freq, len, amp);
	}
	else if (which >= 3 && which <= 5) // filtered white noise
	{
		// amplified 30dB because the filtering makes what actually
		// comes out very quiet. it would probably be difficult to
		// actually equalize with a sine wave volume-wise, as it's
		// base frequency-dependent.
		snprintf(cmd, buflen,
			"white -len %f -amp %f"
			"|filter -type bp -center %f -q 100"
			"|amp -dB +30",
			len, amp, freq);
	}
/*	else if (which == 6) // rossler
	{
		snprintf(cmd, buflen, "rossler -freq %f -len %f -amp %f",
			freq, len, amp);
	}
rossler messes up if freq is above a certain amount, sadly
*/
	else errx(1, "oops, not enough generators");
}

// subdiv is included here so that beat-oriented effects
// can use the subdivision size as the smallest length of time
// to work with

static void add_effect(char *cmd, const size_t buflen, int subdiv)
{
	size_t len;
	size_t spaceleft;
	char *p;
	int which;

	len = strlen(cmd);
	spaceleft = buflen - len;
	p = cmd + len;

	which = rnd(5);
	if (which == 0) // delay
	{
		float delaylen, feedback, wetout;
		wetout = frnd()*100.0;
		//delaylen = frnd()*4500.0+10.0;
		delaylen = beatlength/MAX(4,subdiv) * (rnd(4*MAX(4,subdiv))+1);
		feedback = frnd()*199.8 - 99.9;
		snprintf(p, spaceleft,
			"|delay -len %f -feedback %f -wetout %f",
			delaylen, feedback, wetout);
	}
	else if (which == 1) // compressor
	{
		float threshdB, ratio, attack, release;
		int rms = rnd(2);
		threshdB = (rms ? -12 : -6) - 30*frnd();
		// note: comp program is backwards as of this writing
		// and >1 = expander <1 = compressor
		// though it should probably be the reverse.
		ratio = 1.0 + frnd()*12.0;
		if (rnd(8)) // far more likely to compress than expand
			ratio = 1.0/ratio;
		attack = frnd()*100.0+10.0;
		release = frnd()*1000.0+100.0;
		snprintf(p, spaceleft, "|comp %s -threshdB %f -ratio %f "
			"-attack %f -release %f",
			rms ? "-rms" : "", threshdB, ratio, attack, release);
	}
	else if (which == 2) // exponential distort
	{
		float exponent;
		exponent = frnd()*3 + 1.0;
		if (rnd(2))
			exponent = 1.0 / exponent;
		snprintf(p, spaceleft, "|power -exp %f", exponent);
	}
	else if (which == 3) // beat-creating delay
	{
		float hitlen, decaylen, repeatlen, feedback;
		hitlen = frnd()*0.08 + 0.002;
		decaylen = frnd()*0.02 + 0.0005;
		repeatlen = beatlength/subdiv * (rnd(4*subdiv)+1);
		feedback = frnd()*199.8 - 99.9;
		snprintf(p, spaceleft,
			"|envelope 1 %f 1 %f 0"
			"|delay -len %f -feedback %f -wetout 100",
			hitlen*1000, decaylen*1000,
			repeatlen, feedback);
	}
	else if (which == 4) // sample-and-hold
	{
		int bins;
		float grainlen;

		grainlen = beatlength/subdiv * pow(2, rnd(5));
		bins = rnd(20)+10;
		snprintf(p, spaceleft, "|sampbins -numbins %d -grainlen %f",
			bins, grainlen*1000);
	}
	else errx(1, "oops, not enough effects");
}

static FILE *generate_pipe_source(int *pmaxsamps, int *preplacesamps)
{
	char cmdstring[10240];
	float sinefreq;
	float secs; // sound length in seconds
	float panangle;
	float infadetime, outfadetime;
	float amp;
	int subdiv, subdepth, subbeats; /* see SUBDIV above */
	int note;
	int interval;
	int harmonic;
	int ix;
	int numfx;
	const char *infadetype, *outfadetype;
	FILE *fp;

	// decide on a length for the sound
	subdepth = rnd(SUBPOT+1); // given SUBPOT=5, this is 0 to 5
	subdiv = (int)pow(2, subdepth); // 1, 2, 4, 8, 16, or 32
	subbeats = rnd((maxbeats*subdiv) + 1 - (minbeats*subdiv))
		+ (minbeats*subdiv);
	subbeats *= SUBDIV/subdiv;
	secs = beatlength/SUBDIV * subbeats;

	*pmaxsamps = (int)(RATE*secs + 0.99);

	// melody/harmony/note value:
	// decide how many steps to go, preferring smaller steps
	// (or no change at all)
	interval = rnd(2);
	for (ix = 0; ix < 5; ix++)
		interval = rnd(2+interval);

	// go up or down this many steps
	note = stepsfrom(lastnote, interval*(rnd(2) ? -1 : 1));

	if (note < 0)
		note += 12;
	if (note > SEMIRANGE)
		note -= 12;

	lastnote = note;

	sinefreq = CENTERNOTE * pow(2, (note-SEMIRANGE/2)/12.0);
	panangle = rnd(90000) / 1000.0 - 45.0;
	amp = DBTORAT(-12 - rnd(12));

	// sometimes make harmonics instead of base freq
	harmonic = rnd(5)+1;
	sinefreq *= harmonic;
	amp /= harmonic;

	infadetime = rnd(10000)/10000.0*(secs*0.5) + (secs*0.25);
	infadetime = MAX(0.1, infadetime);
	outfadetime = rnd(10000)/10000.0*(secs*0.5) + (secs*0.25);
	outfadetime = MAX(0.1, outfadetime);
	infadetype = fadetypes[rnd(sizeof fadetypes / sizeof fadetypes[0])];
	outfadetype = fadetypes[rnd(sizeof fadetypes / sizeof fadetypes[0])];

	*preplacesamps = (int)((secs - outfadetime) * RATE);

	// XXX note in the following, with each program,
	// I generate 0.2 sec extra since I think inexactness
	// here, in terms of length, may be the cause of occasional clicks?
	// But then the clicks still come so I don't know why.

	install_gen(cmdstring, sizeof cmdstring, sinefreq, (secs+0.2)*1000,
		amp);

	numfx = rnd(4)+2;
	for (ix = 0; ix < numfx; ix++)
		add_effect(cmdstring, sizeof cmdstring, subdiv);

	snprintf(cmdstring + strlen(cmdstring),
		sizeof cmdstring - strlen(cmdstring),
		"|pan -angle %f"
		"|fadef in %s 0 %f | fadef out %s %f %f",
		panangle,
		infadetype, infadetime, outfadetype, secs - outfadetime,
		secs+0.02 /* extra 0.02s to hopefully avoid clicks */);

//	snprintf(cmdstring, sizeof cmdstring,
//		"sine -freq %f -len %f -amp %f | pan -angle %f "
//		"| fadef in %s 0 %f | fadef out %s %f %f",
//		sinefreq, (secs+0.2)*1000, amp, panangle,
//		infadetype, infadetime, outfadetype, secs - outfadetime, secs);
	fprintf(stderr, "new sound source: %s\n", cmdstring);//Tgk
	fp = popen(cmdstring, "r");
	if (fp == NULL)
		err(1, "opening command [[[%s]]] failed", cmdstring);
	return fp;
}

static void del_sndsrc(int num)
{
	sndsrc_t *snd = sndsrcs[num];
	pclose(snd->fp);
	free(snd);
	nsndsrcs--;
	memmove(&sndsrcs[num], &sndsrcs[num+1],
		(sizeof sndsrcs[0]) * (nsndsrcs - num));
}

static void add_sndsrc(void)
{
	sndsrc_t *snd;

	XPND(sndsrcs, nsndsrcs, ssndsrcs);
	snd = xm(sizeof *snd, 1);
	sndsrcs[nsndsrcs++] = snd;

	snd->fp = generate_pipe_source(&snd->maxsamps, &snd->replacesamps);
	// XXX snd->maxsamps and snd->replacesamps
	// are populated in a weird way thru subfunction
	snd->replaced = 0;
	snd->sampcount = 0;
}

static int read_sndsrc(int num, float *dest)
{
	int ret;
	ret = fread(dest, sizeof *dest, 2, sndsrcs[num]->fp);
	if (ret < 2)
	{
		dest[0] = dest[1] = 0.0;
		return 0;
	}
	return 1;
}

static void make_sound(float bpm, int samplen, int initsnds,
	int minlen, int maxlen)
{
	int ix;
	int sampsout;

	beatlength = 60 / bpm;
	minbeats = (int)((float)minlen / RATE / beatlength);
	maxbeats = (int)((float)maxlen / RATE / beatlength);

	for (ix = 0; ix < initsnds; ix++)
		add_sndsrc();

	for (sampsout = 0; sampsout < samplen; sampsout++)
	{
		float sum[2] = {0.0, 0.0};
		float f[2];
		int ret;

		for (ix = 0; ix < nsndsrcs; ix++)
		{
			ret = read_sndsrc(ix, f);
			sndsrcs[ix]->sampcount++;
			if (sndsrcs[ix]->sampcount >= sndsrcs[ix]->maxsamps
				|| ret == 0 /* EOF */)
			{
				if (!sndsrcs[ix]->replaced)
					add_sndsrc();
				del_sndsrc(ix);
				ix--;
			}
			else if (sndsrcs[ix]->sampcount
				>= sndsrcs[ix]->replacesamps
				&& !sndsrcs[ix]->replaced)
			{
				add_sndsrc();
				sndsrcs[ix]->replaced = 1;
			}
			sum[0] += f[0];
			sum[1] += f[1];
		}

		if (fwrite(sum, sizeof sum[0], 2, stdout) < 2)
			exit(0);
	}
}

int main(void)
{
	if (isatty(STDOUT_FILENO))
		errx(1, "stdout should not be a terminal");

	get_rate();

	// XXX hardcoded random bounds
	make_sound(
		frnd()*110.0+70.0, // 70 to 180 bpm
		rnd(RATE*421) + RATE*120, // 2 to 9 minutes
		rnd(5)+4, // 4 to 8 initial sndsrcs
		rnd(RATE*27) + RATE*4, // minimum length 4-30 sec
		rnd(RATE*56) + RATE*35 // max length 35-90 sec
	);

	return 0;
}
