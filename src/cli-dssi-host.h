/* cli-dssi-host.h
 * Copyright (C) 2005 James McDermott
 * jamesmichaelmcdermott@gmail.com
 *
 * This program is derived from jack-dssi-host 
 * (Copyright 2004 Chris Cannam, Steve Harris and Sean Bolton).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 01222-1307
 * USA
 */

#ifndef _CLI_DSSI_HOST_H
#define _CLI_DSSI_HOST_H

#define _BSD_SOURCE    1
#define _SVID_SOURCE   1
#define _ISOC99_SOURCE 1


#define DEBUG 0
#define MAX_LENGTH (15.0f)
#define SAMPLE_RATE 44100
/* character used to separate SO names from plugin labels on command line */
#define LABEL_SEP ':'
#define KEYVAL_SEP '='
#define BANK_SEP ':'

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sndfile.h>
#include <ladspa.h>
#include <dssi.h>
#include <alsa/asoundlib.h>
#include <alsa/seq.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <dlfcn.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <libgen.h>


static float sample_rate;
static int verbose = 0;
char *my_name;

LADSPA_Data get_port_random(const LADSPA_Descriptor *plugin, int port)
{
  LADSPA_PortRangeHint hint = plugin->PortRangeHints[port];
  float lower = hint.LowerBound *
    (LADSPA_IS_HINT_SAMPLE_RATE(hint.HintDescriptor) ? sample_rate : 1.0f);
  float upper = hint.UpperBound *
    (LADSPA_IS_HINT_SAMPLE_RATE(hint.HintDescriptor) ? sample_rate : 1.0f);

  /* FIXME: here we assume that ports are bounded, and we do not take the 
   * logarithmic hint into account. */

  float x = random() / (float) RAND_MAX;

  return lower + x * (upper - lower);
}

LADSPA_Data get_port_default(const LADSPA_Descriptor *plugin, int port)
{
  LADSPA_PortRangeHint hint = plugin->PortRangeHints[port];
  float lower = hint.LowerBound *
    (LADSPA_IS_HINT_SAMPLE_RATE(hint.HintDescriptor) ? sample_rate : 1.0f);
  float upper = hint.UpperBound *
    (LADSPA_IS_HINT_SAMPLE_RATE(hint.HintDescriptor) ? sample_rate : 1.0f);

  if (!LADSPA_IS_HINT_HAS_DEFAULT(hint.HintDescriptor)) {
    if (!LADSPA_IS_HINT_BOUNDED_BELOW(hint.HintDescriptor) ||
	!LADSPA_IS_HINT_BOUNDED_ABOVE(hint.HintDescriptor)) {
      /* No hint, its not bounded, wild guess */
      return 0.0f;
    }

    if (lower <= 0.0f && upper >= 0.0f) {
      /* It spans 0.0, 0.0 is often a good guess */
      return 0.0f;
    }

    /* No clues, return minimum */
    return lower;
  }

  /* Try all the easy ones */
    
  if (LADSPA_IS_HINT_DEFAULT_0(hint.HintDescriptor)) {
    return 0.0f;
  } else if (LADSPA_IS_HINT_DEFAULT_1(hint.HintDescriptor)) {
    return 1.0f;
  } else if (LADSPA_IS_HINT_DEFAULT_100(hint.HintDescriptor)) {
    return 100.0f;
  } else if (LADSPA_IS_HINT_DEFAULT_440(hint.HintDescriptor)) {
    return 440.0f;
  }

  /* All the others require some bounds */

  if (LADSPA_IS_HINT_BOUNDED_BELOW(hint.HintDescriptor)) {
    if (LADSPA_IS_HINT_DEFAULT_MINIMUM(hint.HintDescriptor)) {
      return lower;
    }
  }
  if (LADSPA_IS_HINT_BOUNDED_ABOVE(hint.HintDescriptor)) {
    if (LADSPA_IS_HINT_DEFAULT_MAXIMUM(hint.HintDescriptor)) {
      return upper;
    }
    if (LADSPA_IS_HINT_BOUNDED_BELOW(hint.HintDescriptor)) {
      if (LADSPA_IS_HINT_LOGARITHMIC(hint.HintDescriptor) &&
	  lower > 0.0f && upper > 0.0f) {
	if (LADSPA_IS_HINT_DEFAULT_LOW(hint.HintDescriptor)) {
	  return expf(logf(lower) * 0.75f + logf(upper) * 0.25f);
	} else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(hint.HintDescriptor)) {
	  return expf(logf(lower) * 0.5f + logf(upper) * 0.5f);
	} else if (LADSPA_IS_HINT_DEFAULT_HIGH(hint.HintDescriptor)) {
	  return expf(logf(lower) * 0.25f + logf(upper) * 0.75f);
	}
      } else {
	if (LADSPA_IS_HINT_DEFAULT_LOW(hint.HintDescriptor)) {
	  return lower * 0.75f + upper * 0.25f;
	} else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(hint.HintDescriptor)) {
	  return lower * 0.5f + upper * 0.5f;
	} else if (LADSPA_IS_HINT_DEFAULT_HIGH(hint.HintDescriptor)) {
	  return lower * 0.25f + upper * 0.75f;
	}
      }
    }
  }

  /* fallback */
  return 0.0f;
}




char *
load(const char *dllName, void **dll, int quiet) /* returns directory where dll found */
{
  static char *defaultDssiPath = 0;
  const char *dssiPath = getenv("DSSI_PATH");
  char *path, *origPath, *element;
  const char *message;
  void *handle = 0;

  /* If the dllName is an absolute path */
  if (*dllName == '/') {
    if ((handle = dlopen(dllName, RTLD_NOW))) {  /* real-time programs should not use RTLD_LAZY */
      *dll = handle;
      path = strdup(dllName);
      return dirname(path);
    } else {
      if (!quiet) {
	fprintf(stderr, "%s: Error: Cannot find DSSI or LADSPA plugin at '%s'\n", my_name, dllName);
      }
      return NULL;
    }
  }

  if (!dssiPath) {
    if (!defaultDssiPath) {
      const char *home = getenv("HOME");
      if (home) {
	defaultDssiPath = malloc(strlen(home) + 60);
	sprintf(defaultDssiPath, "/usr/local/lib/dssi:/usr/lib/dssi:%s/.dssi", home);
      } else {
	defaultDssiPath = strdup("/usr/local/lib/dssi:/usr/lib/dssi");
      }
    }
    dssiPath = defaultDssiPath;
    if (!quiet) {
      fprintf(stderr, "\n%s: Warning: DSSI path not set\n%s: Defaulting to \"%s\"\n\n", my_name, my_name, dssiPath);
    }
  }

  path = strdup(dssiPath);
  origPath = path;
  *dll = 0;

  while ((element = strtok(path, ":")) != 0) {

    char *filePath;

    path = 0;

    if (element[0] != '/') {
      if (!quiet) {
	fprintf(stderr, "%s: Warning: Ignoring relative element \"%s\" in path\n", my_name, element);
      }
      continue;
    }

    if (!quiet && verbose) {
      fprintf(stderr, "%s: Looking for library \"%s\" in %s... ", my_name, dllName, element);
    }

    filePath = (char *)malloc(strlen(element) + strlen(dllName) + 2);
    sprintf(filePath, "%s/%s", element, dllName);

    if ((handle = dlopen(filePath, RTLD_NOW))) {  /* real-time programs should not use RTLD_LAZY */
      if (!quiet && verbose) {
	fprintf(stderr, "found\n");
      }
      *dll = handle;
      free(filePath);
      path = strdup(element);
      free(origPath);
      return path;
    }

    if (!quiet && verbose) {
      message = dlerror();
      if (message) {
	fprintf(stderr, "not found: %s\n", message);
      } else {
	fprintf(stderr, "not found\n");
      }
    }

    free(filePath);
  }

  free(origPath);
  return 0;
}

int
is_silent(LADSPA_Data *data, size_t length) {
  float epsilon = 0.01f;
  float sum = 0.0f;

  for (size_t i = 0; i < length; i++) {
    sum += fabs(data[i]);
  }
  return (sum < epsilon);
}

inline int
min(int x, int y) {
  return (x < y) ? x : y;
}


void
parse_keyval(char *input, char sep, char **key, char **val) {
  
  char *tmp = strchr(input, sep);
  if (tmp) {
    *key = calloc(1, tmp - input + 1);
    strncpy(*key, input, tmp - input);
    *val = strdup(tmp + 1);
  } else {
    *key = strdup(input);
    *val = NULL;
  }
}

typedef enum {
  from_stdin,
  from_defaults,
  from_preset,
  from_random
} port_vals_source_t;

#endif /* _CLI_DSSI_HOST_H */

