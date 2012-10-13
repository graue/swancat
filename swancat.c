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

static int lastnote = SEMIRANGE/2+3; // 3 above A-4 (440 Hz) = C-5, I guess

static const char *fadetypes[] = {"lin", "log", "cos", "logcos"};

// which notes are included in the scale used
// 0 = note not included, 1 = note included
//was                      A   B C   D   E F   G     = A minor / C major?
// static int scale[12] = {1,0,1,1,0,1,0,1,1,0,1,0};

static int scale[12] = {1,0,1,0,1,0,0,1,0,1,0,0}; // A major pentatonic

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

static FILE *generate_pipe_source(int maxsamps, int *preplacesamps)
{
	char cmdstring[10240];
	float sinefreq;
	float secs; // sine length in seconds
	float panangle;
	float infadetime, outfadetime;
	float amp;
	int note;
	int interval;
	int harmonic;
	int ix;
	const char *infadetype, *outfadetype;
	FILE *fp;

	// decide how many steps to go, preferring smaller steps
	// (or no change at all)
	interval = rnd(2);
	for (ix = 0; ix < 5; ix++)
		interval = rnd(2+interval);

	// go up or down this many steps
	fprintf(stderr, "last note: %d, interval: %d\n", lastnote, interval);//Tgk
	note = stepsfrom(lastnote, interval*(rnd(2) ? -1 : 1));

	if (note < 0)
		note += 12;
	if (note > SEMIRANGE)
		note -= 12;

	lastnote = note;

	sinefreq = CENTERNOTE * pow(2, (note-SEMIRANGE/2)/12.0);
	secs = (float)maxsamps / RATE;
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

	// XXX note in the following, with the sine program,
	// I use a sine length 0.05 sec too long since I think inexactness
	// here, in terms of length, may be the cause of occasional clicks?
	// But then the clicks still come so I don't know why.

	snprintf(cmdstring, sizeof cmdstring,
		"sine -freq %f -len %f -amp %f | pan -angle %f "
		"| fadef in %s 0 %f | fadef out %s %f %f",
		sinefreq, (secs+0.05)*1000, amp, panangle,
		infadetype, infadetime, outfadetype, secs - outfadetime, secs);
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

static void add_sndsrc(int minlen, int maxlen)
{
	sndsrc_t *snd;

	XPND(sndsrcs, nsndsrcs, ssndsrcs);
	snd = xm(sizeof *snd, 1);
	sndsrcs[nsndsrcs++] = snd;

	snd->maxsamps = rnd(maxlen - minlen + 1) + minlen;
	snd->fp = generate_pipe_source(snd->maxsamps, &snd->replacesamps);
	// XXX snd->replacesamps populated in a weird way thru subfunction
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

static void make_sound(int samplen, int initsnds, int minlen, int maxlen)
{
	int ix;
	int sampsout;

	for (ix = 0; ix < initsnds; ix++)
		add_sndsrc(minlen, maxlen);

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
					add_sndsrc(minlen, maxlen);
				del_sndsrc(ix);
				ix--;
			}
			else if (sndsrcs[ix]->sampcount
				>= sndsrcs[ix]->replacesamps
				&& !sndsrcs[ix]->replaced)
			{
				add_sndsrc(minlen, maxlen);
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

	// XXX hardcoded parameters
	make_sound(
		RATE * 300, // 5 minutes
		4, // initial sndsrcs
		RATE * 5, // minimum length 5 sec
		RATE * 62 // max length 62 sec
	);

	return 0;
}
