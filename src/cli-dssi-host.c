/* cli-dssi-host.c
 * Copyright (C) 2005 James McDermott
 * jamesmichaelmcdermott@gmail.com
 *
 * This program is derived from jack-dssi-host (Copyright 2004 Chris
 * Cannam, Steve Harris and Sean Bolton).
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

#include "cli-dssi-host.h"

void
print_usage(void) {
  fprintf(stderr, "A command-line DSSI host.\n");
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "$ %s <dssi_plugin.so>[%c<label>]\n", my_name, LABEL_SEP);
  fprintf(stderr, "  [-p [<bank>%c]<preset>] "
	  "(use -p -1 for default port values;\n           "
	  "-p -2 for random values; omit -p to read port values from stdin)\n",
	  BANK_SEP);
  fprintf(stderr, "  [-l <length>] (in seconds, between note-on and note-off; default is 1s)\n");
  fprintf(stderr, "  [-r <release_tail>] (in seconds: amount of data to allow after note-off;\n           default waits until silence (up to a maximum of 15s))\n");
  fprintf(stderr, "  [-f <output_file.wav>] (default == \"output.wav\")\n");
  fprintf(stderr, "  [-c <no_channels>] (default == 1; use -c -1 to use plugin's channel count)\n");
  fprintf(stderr, "  [-n <midi_note_no>] (default == 60)\n");
  fprintf(stderr, "  [-v <midi_velocity>] (default == 127)\n");
  fprintf(stderr, "  [-d <project_directory>]\n");
  fprintf(stderr, "  [-k <configure_key>%c<value>] ...\n", KEYVAL_SEP);
  fprintf(stderr, "  [-b] (clip out-of-bounds values, including Inf and NaN, to within bounds\n       (calls exit()) if -b is omitted)\n");
  exit(1);
}
  
int
main(int argc, char **argv) {

  my_name = basename(argv[0]);

  DSSI_Descriptor_Function descfn;
  const DSSI_Descriptor *descriptor;
  LADSPA_Handle instanceHandle;
  void *pluginObject;

  SNDFILE *outfile;
  SF_INFO outsfinfo;

  char **configure_key = NULL;
  char **configure_val = NULL;
  char *directory = NULL;
  char *dllName = NULL;
  char *label;
  char *output_file = "output.wav";
  char *projectDirectory = NULL;

  int in, out, controlIn, controlOut;
  int ins, outs, controlIns, controlOuts;
  port_vals_source_t src = from_stdin;
  int nchannels = 1;
  int midi_velocity = 127;
  int midi_note = 60;
  int bank = 0;
  int program_no = 0;
  int nkeys = 0;
  int clip = 0;
  int have_warned = 0;


  size_t length = SAMPLE_RATE;
  size_t release_tail = -1;
  size_t nframes = 256;
  size_t total_written = 0;
  size_t items_written = 0;
  
  float **pluginInputBuffers, **pluginOutputBuffers;
  float *pluginControlIns, *pluginControlOuts;

  sample_rate = SAMPLE_RATE;
  
  /* Probably an unorthodox srandom() technique... */
  struct timeval tv;
  struct timezone tz;
  gettimeofday(&tv, &tz);
  srandom(tv.tv_sec + tv.tv_usec);

  if (argc < 2) {
    print_usage();
  }

  /* dll name is argv[1]: parse dll name, plus a label if supplied */
  parse_keyval(argv[1], LABEL_SEP, &dllName, &label);

  for (int i = 2; i < argc; i++) {
    if (DEBUG) {
      fprintf(stderr, "%s: processing options: argv[%d] = %s\n", 
	      my_name, i, argv[i]);
    }

    /* Deal with flags */
    if (!strcmp(argv[i], "-b")) {
      clip = 1;
      continue;
    } else {
      /* It's not a flag, so expect option + argument */
      if (argc <= i + 1) print_usage();
    }

    if (!strcmp(argv[i], "-c")) {
      nchannels = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "-f")) {
      output_file = argv[++i];
    } else if (!strcmp(argv[i], "-n")) {
      midi_note = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "-v")) {
      midi_velocity = strtol(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "-d")) {
      projectDirectory = argv[++i];
    } else if (!strcmp(argv[i], "-l")) {
      length = sample_rate * strtof(argv[++i], NULL);
    } else if (!strcmp(argv[i], "-r")) {
      release_tail = sample_rate * strtof(argv[++i], NULL);
    } else if (!strcmp(argv[i], "-k")) {
      configure_key = realloc(configure_key, (nkeys + 1) * sizeof(char *));
      configure_val = realloc(configure_val, (nkeys + 1) * sizeof(char *));
      parse_keyval(argv[++i], KEYVAL_SEP, &configure_key[nkeys], 
		   &configure_val[nkeys]);
      nkeys++;
      
    } else if (!strcmp(argv[i], "-p")) {
      char *first_str;
      char *second_str;
      parse_keyval(argv[++i], BANK_SEP, &first_str, &second_str);
      if (second_str) {
	bank = strtol(first_str, NULL, 0);
	program_no = strtol(second_str, NULL, 0);
      } else {
	program_no = strtol(first_str, NULL, 0);
	bank = 0;
      }
      if (program_no == -1) {
	src = from_defaults;
      } else if (program_no == -2) {
	src = from_random;
      } else {
	src = from_preset;
      }
    } else {
      fprintf(stderr, "%s: Error: Unknown option: %s\n", my_name, argv[i]);
      print_usage();
    }
  }

  if (DEBUG) {
    for (int i = 0; i < nkeys; i++) {
      printf("key %d: %s; value: %s\n", i, configure_key[i],
	     configure_val[i]);
    }
  }

  if (DEBUG) {
    fprintf(stderr, "%s: Cmd-line args ok\n", my_name);
  }

  directory = load(dllName, &pluginObject, 0);
  if (!directory || !pluginObject) {
    fprintf(stderr, "\n%s: Error: Failed to load plugin library \"%s\"\n", 
	    my_name, dllName);
    return 1;
  }
                
  descfn = (DSSI_Descriptor_Function)dlsym(pluginObject, 
					   "dssi_descriptor");

  if (!descfn) {
    fprintf(stderr, "%s: Error: Not a DSSI plugin\n", my_name);
    exit(1);
  }


  /* Get the plugin descriptor and check the run_synth*() function
   * exists */
  int j = 0;
  descriptor = NULL;
  const DSSI_Descriptor *desc;
	
  while ((desc = descfn(j++))) {
    if (!label ||
	!strcmp(desc->LADSPA_Plugin->Label, label)) {
      descriptor = desc;
      break;
    }
  }

  if (!descriptor) {
    fprintf(stderr, 
	    "\n%s: Error: Plugin label \"%s\" not found in library \"%s\"\n",
	    my_name, label ? label : "(none)", dllName);
    return 1;
  }

  if (!descriptor->run_synth 
      && !descriptor->run_multiple_synths) {
    fprintf(stderr, "%s: Error: No run_synth() or run_multiple_synths() method in plugin\n", my_name);
    exit(1);
  }

  if (!label) {
    label = strdup(descriptor->LADSPA_Plugin->Label);
  }

  /* Count number of i/o buffers and ports required */
  ins = 0;
  outs = 0;
  controlIns = 0;
  controlOuts = 0;
 
  for (int j = 0; j < descriptor->LADSPA_Plugin->PortCount; j++) {
    LADSPA_PortDescriptor pod =
      descriptor->LADSPA_Plugin->PortDescriptors[j];

    if (LADSPA_IS_PORT_AUDIO(pod)) {
	    
      if (LADSPA_IS_PORT_INPUT(pod)) ++ins;
      else if (LADSPA_IS_PORT_OUTPUT(pod)) ++outs;
	    
    } else if (LADSPA_IS_PORT_CONTROL(pod)) {
	    
      if (LADSPA_IS_PORT_INPUT(pod)) ++controlIns;
      else if (LADSPA_IS_PORT_OUTPUT(pod)) ++controlOuts;
    }
  }

  if (!outs) {
    fprintf(stderr, "%s: Error: no audio output ports\n", my_name);
    exit(1);
  }
  if (nchannels == -1) {
    nchannels = outs;
  }

  /* Create buffers */

  pluginInputBuffers = (float **)malloc(ins * sizeof(float *));
  pluginControlIns = (float *)calloc(controlIns, sizeof(float));

  pluginOutputBuffers = (float **)malloc(outs * sizeof(float *));
  pluginControlOuts = (float *)calloc(controlOuts, sizeof(float));

  for (int i = 0; i < outs; i++) {
    pluginOutputBuffers[i] = (float *)calloc(nframes, sizeof(float));
  }



  /* Instantiate plugin */

  instanceHandle = descriptor->LADSPA_Plugin->instantiate
    (descriptor->LADSPA_Plugin, sample_rate);
  if (!instanceHandle) {
    fprintf(stderr, 
	    "\n%s: Error: Failed to instantiate instance %d!, plugin \"%s\"\n",
	    my_name, 0, label);
    return 1;
  }

  /* Connect ports */

  in = out = controlIn = controlOut = 0;
  for (int j = 0; j < descriptor->LADSPA_Plugin->PortCount; j++) {  
    /* j is LADSPA port number */
    
    LADSPA_PortDescriptor pod =
      descriptor->LADSPA_Plugin->PortDescriptors[j];
    
    if (LADSPA_IS_PORT_AUDIO(pod)) {
      
      if (LADSPA_IS_PORT_INPUT(pod)) {
	descriptor->LADSPA_Plugin->connect_port
	  (instanceHandle, j, pluginInputBuffers[in++]);
	
      } else if (LADSPA_IS_PORT_OUTPUT(pod)) {
	descriptor->LADSPA_Plugin->connect_port
	  (instanceHandle, j, pluginOutputBuffers[out++]);
      }
      
    } else if (LADSPA_IS_PORT_CONTROL(pod)) {
      
      if (LADSPA_IS_PORT_INPUT(pod)) {
	
	descriptor->LADSPA_Plugin->connect_port
	  (instanceHandle, j, &pluginControlIns[controlIn++]);
	
      } else if (LADSPA_IS_PORT_OUTPUT(pod)) {
	descriptor->LADSPA_Plugin->connect_port
	  (instanceHandle, j, &pluginControlOuts[controlOut++]);
      }
    }
  }  /* 'for (j...'  LADSPA port number */



  /* Set the control port values */

  if (src == from_preset) {
    /* Set the ports according to a preset */
    if (descriptor->select_program) {
      descriptor->select_program(instanceHandle, bank, program_no);
    }
  } else {
    /* Assign values to control ports: defaults, random, or from stdin */
    controlIn = 0;
    for (int j = 0; j < descriptor->LADSPA_Plugin->PortCount; j++) {  
      /* j is LADSPA port number */
      
      LADSPA_PortDescriptor pod =
	descriptor->LADSPA_Plugin->PortDescriptors[j];
      
      if (LADSPA_IS_PORT_CONTROL(pod) && LADSPA_IS_PORT_INPUT(pod)) {
	LADSPA_Data val;
	if (src == from_defaults) {
	  val = get_port_default(descriptor->LADSPA_Plugin, j);
	} else if (src == from_stdin) {
	  scanf("%f", &val);
	} else if (src == from_random) {
	  val = get_port_random(descriptor->LADSPA_Plugin, j);
	}
	pluginControlIns[controlIn] = val;
	controlIn++;
      }
    }
  }
	

  /* It can happen that a control port is set wrongly after
   * select_program(): for example xsynth-dssi does not set its tuning
   * port in the select_program() call (which makes sense: xsynth
   * users might want to be able to keep their current tuning while
   * changing presets).  Here, if we call select_program() we'll get
   * tuning = 0.0, and we don't get any sound. There might be other
   * bad effects in other cases.  One solution is to read all the
   * control-in values and if they're not in range, reset them using
   * get_default().
   */

  controlIn = 0;
  for (int j = 0; j < descriptor->LADSPA_Plugin->PortCount; j++) {  
    /* j is LADSPA port number */
      
    LADSPA_PortDescriptor pod =
      descriptor->LADSPA_Plugin->PortDescriptors[j];
    
    if (LADSPA_IS_PORT_CONTROL(pod) && LADSPA_IS_PORT_INPUT(pod)) {

      LADSPA_PortRangeHintDescriptor prhd =
	descriptor->LADSPA_Plugin->PortRangeHints[j].HintDescriptor;      
      const char * pname = descriptor->LADSPA_Plugin->PortNames[j];
      LADSPA_Data lb = descriptor->LADSPA_Plugin->
	PortRangeHints[j].LowerBound;
      LADSPA_Data ub = descriptor->LADSPA_Plugin->
	PortRangeHints[j].UpperBound;
      LADSPA_Data val = pluginControlIns[controlIn];
      LADSPA_Data def = get_port_default(descriptor->LADSPA_Plugin, j);

      if ((LADSPA_IS_HINT_BOUNDED_BELOW(prhd) && val < lb) ||
	  (LADSPA_IS_HINT_BOUNDED_ABOVE(prhd) && val > ub)) {
	fprintf(stderr, 
		"%s: Warning: port %d (%s) was %.3f, overriding to %.3f\n",
		my_name, j, pname, val, def);
	pluginControlIns[controlIn] = def;
      }
      if (DEBUG) {
	fprintf(stderr, 
		"port %3d; prhd %3d; lb %6.2f; ub %6.2f; val %6.2f (%s)\n",
		j, prhd, lb, ub, val, pname);
      }
      controlIn++;
    }
  }

  /* Activate */

  if (descriptor->LADSPA_Plugin->activate) {
    descriptor->LADSPA_Plugin->activate(instanceHandle);
  }


  /* Configure */

  if (projectDirectory && descriptor->configure) {
    char *rv = descriptor->configure(instanceHandle,
				     DSSI_PROJECT_DIRECTORY_KEY,
				     projectDirectory);
    if (rv) {
      fprintf(stderr, 
	      "%s: Warning: plugin doesn't like project directory: \"%s\"\n",
	      my_name, rv);
    }
  }
  if (nkeys && descriptor->configure) {
    for (int i = 0; i < nkeys; i++) {
      char *rv = descriptor->configure(instanceHandle,
				       configure_key[i],
				       configure_val[i]);
      if (rv) {
	fprintf(stderr, 
		"%s: Warning: plugin doesn't like "
		"configure key-value pair: \"%s\"\n", 
		my_name, rv);
      }
    }
  }


  /* Open sndfile */

  outsfinfo.samplerate = sample_rate;
  outsfinfo.channels = nchannels;
  outsfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  outsfinfo.frames = length;
    
  outfile = sf_open(output_file, SFM_WRITE, &outsfinfo);
  if (!outfile) {
    fprintf(stderr, "%s: Error: Not able to open output file %s.\n", 
	    my_name, output_file);
    fprintf(stderr, "%s: %s\n", my_name, sf_strerror(outfile));
    return 1;
  }


  /* Instead of creating an alsa midi input, we fill in two events
   * note-on and note-off */

  snd_seq_event_t on_event, off_event, *current_event;
  on_event.type = SND_SEQ_EVENT_NOTEON;
  on_event.data.note.channel = 0;
  on_event.data.note.note = midi_note;
  on_event.data.note.velocity = midi_velocity;
  on_event.time.tick = 0;

  off_event.type = SND_SEQ_EVENT_NOTEOFF;
  off_event.data.note.channel = 0;
  off_event.data.note.note = midi_note;
  off_event.data.note.off_velocity = midi_velocity;
  off_event.time.tick = 0;

  /* Generate the data: send an on-event, wait, send an off-event, 
     wait for release tail to die */
  total_written = 0;
  int finished = 0;
  unsigned long nevents;
  while (!finished) {
    if (total_written == 0) {
      current_event = &on_event;
      nevents = 1;
    } else if (total_written >= length && total_written < length + nframes) {
      current_event = &off_event;
      nevents = 1;
    } else {
      current_event = NULL;
      nevents = 0;
    }

/*     if (DEBUG) { */
/*       fprintf(stderr, "about to call run_synth() or run_multiple_synths() with %ld events\n", nevents); */
/*     } */

    if (descriptor->run_synth) {
      descriptor->run_synth(instanceHandle,
			    nframes,
			    current_event,
			    nevents);
    } else if (descriptor->run_multiple_synths) {
      descriptor->run_multiple_synths(1,
				      &instanceHandle,
				      nframes,
				      &current_event,
				      &nevents);
    }

    /* Interleaving for libsndfile. */
    float sf_output[nchannels * nframes];
    for (int i = 0; i < nframes; i++) {
      /* First, write all the obvious channels */
      for (int j = 0; j < min(outs, nchannels); j++) {
	/* If outs > nchannels, we *could* do mixing - but don't. */
	sf_output[i * nchannels + j] = pluginOutputBuffers[j][i];
      }
      /* Then, if user wants *more* output channels than there are
       * audio output ports (ie outs < nchannels), copy the last audio
       * out to all the remaining channels. If outs >= nchannels, this
       * loop is never entered. */
      for (int j = outs; j < nchannels; j++) {
	sf_output[i * nchannels + j] = pluginOutputBuffers[outs - 1][i];
      }
    }

    if (clip) {
      for (int i = 0; i < nframes * nchannels; i++) {
	if (!finite(sf_output[i])) {
	  if (!have_warned) {
	    have_warned = 1;
	    fprintf(stderr, 
		    "%s: Warning: clipping NaN or Inf in synthesized data\n", 
		    my_name);
	  }
	  if (sf_output[i] < 0.0f) {
	    sf_output[i] = -1.0f;
	  } else {
	    sf_output[i] = 1.0f;
	  }
	} else {
	  if (sf_output[i] < -1.0f) {
	    if (!have_warned) {
	      have_warned = 1;
	      fprintf(stderr, 
		      "%s: Warning: clipping out-of-bounds value in synthesized data\n", 
		      my_name);
	    }
	    sf_output[i] = -1.0f;
	  } else if (sf_output[i] > 1.0f) {
	    if (!have_warned) {
	      have_warned = 1;
	      fprintf(stderr, 
		      "%s: Warning: clipping out-of-bounds value in synthesized data\n", 
		      my_name);
	    }
	    sf_output[i] = 1.0f;
	  }
	}
      }
    } else {
      for (int i = 0; i < nframes * nchannels; i++) {
	if (!finite(sf_output[i])) {
	  fprintf(stderr, "%s: Error: NaN or Inf in synthesized data\n",
		  my_name);
	  exit(1);
	}
	if (sf_output[i] > 1.0f
	    || sf_output[i] < -1.0f) {
	  fprintf(stderr, "%s: Error: sample data out of bounds\n",
		  my_name);
	  exit(1);
	}
      }
    }

    /* Write the audio */
    if ((items_written = sf_writef_float(outfile, 
					 sf_output,
					 nframes)) != nframes) {
      fprintf(stderr, "%s: Error: can't write data to output file %s\n", 
	      my_name, output_file);
      fprintf(stderr, "%s: %s\n", my_name, sf_strerror(outfile));
      return 1;
    }
    
    total_written += items_written;
    if (release_tail >= 0) {
      if (total_written > length + release_tail) {
	finished = 1;
      }
    } else {
      if (total_written > length 
	  && is_silent(sf_output, nframes * nchannels)) {
	finished = 1;
      } else if (total_written > MAX_LENGTH * sample_rate) {
	/* The default sineshaper patch never releases, after a note-off,
	 * to silence. So truncate. This is sineshaper 0.3.0 (so maybe it's 
	 * different in the new version) and here I mean the default
	 * patch as returned by the get_port_default() function, not the 
	 * default set by the sineshaper UI.
	 */
	finished = 1;
	fprintf(stderr, "%s: Warning: truncating after writing %d frames\n", 
		my_name, total_written);
      }
    }
  }

  fprintf(stdout, "%s: Wrote %d frames to %s\n", 
	  my_name, total_written, output_file);
    
  sf_close(outfile);
    
  /* Clean up */

  if (descriptor->LADSPA_Plugin->deactivate) {
    descriptor->LADSPA_Plugin->deactivate(instanceHandle);
  }
  
  if (descriptor->LADSPA_Plugin->cleanup) {
    descriptor->LADSPA_Plugin->cleanup(instanceHandle);
  }

  return 0;
}

