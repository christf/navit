.. _configurationadvanced_options:

Configuration/Advanced Options
==============================

.. _advanced_options:

Advanced Options
----------------

The rest of this webpage is meant for advanced/power users who'd like to
fiddle a little more under-the-hood. The average user can safely ignore
this section!

Speech
~~~~~~

Navit can announce driving directions with voice. Navit can use
different mechanisms to play these announcements. Note that not all
tools are available on all platforms. Most engines are launched from a
``cmdline`` speech tag in ``navit.xml``; install and test the engine
outside Navit first.

.. _speech_engines:

Installing speech engines
^^^^^^^^^^^^^^^^^^^^^^^^^

On Debian and Ubuntu, eSpeak can be installed from the repositories:

.. code-block:: bash

   sudo apt install espeak

From espeak 1.45-28 onwards the ``-a`` parameter for volume works. This
is useful because mbrola voices are generally quieter than the default
espeak voice. Espeak 1.44 and newer calls mbrola directly, so the
repository version should be at least 1.44.

Older systems may need a manual build, as was done with Ubuntu Natty:

.. code-block:: bash

   apt-get remove espeak
   apt-get remove espeak-data
   apt-get remove libportaudio0
   apt-get install portaudio19-dev
   apt-get install libportaudio2
   apt-get install debhelper
   unzip espeak-*.zip
   cd /usr/lib64
   ln -s libaudio.so.2 libaudio.so
   cd -
   cd espeak-*/src/
   cp portaudio19.h portaudio.h
   make
   make install
   cd -

The latest espeak sources were published at
`http://espeak.sourceforge.net/ <http://espeak.sourceforge.net/>`__.

Mbrola voices can be installed from the repositories:

.. code-block:: bash

   sudo apt install mbrola

On Debian Bookworm, German voices are available as separate packages:

.. code-block:: bash

   sudo apt install mbrola mbrola-de6 mbrola-de7

Voice packages are listed at `the MBROLA voice page`__ and
`the MBROLA project page <http://tcts.fpms.ac.be/synthesis/mbrola.html>`__.

__ http://www.tcts.fpms.ac.be/synthesis/mbrola/mbrcopybin.html

For manually installed voices, create the destination directories and
place the voice binary (for example ``en1``) in
``/usr/share/mbrola/voices/``:

.. code-block:: bash

   sudo mkdir -p /usr/share/mbrola/voices/

Test a voice directly before configuring Navit:

.. code-block:: bash

   espeak -v mb/mb-en1 "This is mbrola voice english 1"

On Raspbian Jessie, an ``armhf`` package was used:

.. code-block:: bash

   wget http://steinerdatenbank.de/software/mbrola3.0.1h_armhf.deb
   sudo dpkg -i mbrola3.0.1h_armhf.deb

That setup expected the second voice in ``/usr/share/mbrola/en1/``.

.. _prerecorded_samples:

Prerecorded samples
^^^^^^^^^^^^^^^^^^^

Navit is able to compose phrases if you give it a set of prerecorded
**samples**. Configuration example:

.. code-block:: xml

   <speech type="cmdline" data="aplay -q %s"
   sample_dir="/path/to/sampledir" sample_suffix=".wav" flags="1"
   vocabulary_name="0" vocabulary_name_systematic="0" vocabulary_distances="0"/>

The directory *sample_dir* should contain audio files. *sample_suffix*
is the common file type suffix of those files. The names of the files
(without the suffix) must correspond to the text they contain. For each
text it wants to speak, Navit will look for one or more sample files
with corresponding names (ignoring upper/lower case). So for "turn right
in 300 meters" you could use turn.wav, right.wav, in.wav, 300.wav,
meters.wav. Navit will prefer files that contain multiple words: If file
"turn right.wav" is present, it will be used even if you have turn.wav
and right.wav.

Note that Navit internally handles all text in UTF-8 encoding. If you
use a file system where file names are not encoded with UTF-8 (such as
Windows), Navit will only find files for ASCII text. If you use a
language that uses non-ASCII characters, the file name must be the
`percent encoding <http://en.wikipedia.org/wiki/Percent-encoding>`__ of
the UTF-8 representation of the text. For example the filename for "süd"
would be "s%c3%bcd.wav" (because "ü" is encoded as C3BC in UTF-8). For
this feature to work, you must set *flags* to 1.

*data* is the program that can be used to play the sample files. You
should specify the program name along with any necessary parameters. The
placeholder "%s" will be replaced with the file(s) to be played. All
files required for a text will be passed in one go, so the program will
need to support playing multiple files. Note that the %s should *not* be
quoted; the text is not passed through a shell.

Note that if any file that is needed to compose the complete phrase is
missing then Navit will be silent. In that case a warning will be
printed. Unfortunately, there is no complete list of the samples
required. However, all the navigation text is contained in the
translation files (.po files), so you can get a rough list.

By default Navit is trying to announce street names. To disable this
feature you can set *vocabulary_name* and *vocabulary_name_systematic*
to 0 in the speech tag which will specify that the speech synthesizer
isn't capable of speaking names. Also there is *vocabulary_distances*
which you can set to 0 so only the minimum set of
1,2,3,4,5,10,25,50,75,100,150,200,250,300,400,500,750 as numbers is
used.

espeak
^^^^^^

Navit can call espeak directly:

.. code-block:: xml

   <speech type="cmdline" data="espeak -s 150 -v german %s" />

``-s`` sets the speed in words per minute and ``-v`` selects the voice.
The ``%s`` placeholder is filled in by Navit with the phrase to speak.
See :doc:`/user/community/Translations` for language setup.

The call to an external speech command is asynchronous, but it blocks if
the previous phrase is still being spoken. To avoid that, use a wrapper
script with ``&`` at the end of the command:

.. code-block:: xml

   <speech type="cmdline" data="/usr/local/bin/speech-wrapper %s german" />

.. code-block:: bash

   #!/bin/sh
   espeak -s 150 -v "$2" "$1" &

If other programs also use ``/dev/dsp``, route espeak through ALSA
instead:

.. code-block:: xml

   <speech type="cmdline" data="/usr/local/bin/speech-wrapper %s english">

.. code-block:: bash

   #!/bin/sh
   espeak -s 150 -v "$2" --stdout "$1" | aplay > /dev/null

espeak wrapper scripts
^^^^^^^^^^^^^^^^^^^^^^

If you want several arguments, such as playing a tone before every
announcement, or if you use an eSpeak version below 1.44 with a double
pipe, put the speech command in a script and use the script path in the
``data`` attribute of the ``speech`` tag.

For example, ``Speech.sh`` could contain (remember to make it executable
with ``chmod u+x``):

.. code-block:: bash

   #!/bin/bash
   aplay -r 44100 /home/CHANGEME/.navit/sound.wav
   espeak -vmb-en1 -s 150 -a 150 -p 50 "$1"

espeak takes 1-2 seconds to start when the commands are on separate
lines. To start both at once:

.. code-block:: bash

   #!/bin/bash
   aplay -r 44100 /home/CHANGEME/.navit/sound.wav &
   espeak -vmb-en1 -s 150 -a 150 -p 50 "$1"

With an eSpeak version below 1.44, mbrola must be called explicitly:

.. code-block:: bash

   #!/bin/bash
   espeak -vmb-en1 -s 150 -p 45 "$1" | \
     mbrola -e /usr/share/mbrola/voices/en1 - - | \
     aplay -r16550 -fs16 >> /dev/null

``-s`` sets the speed, ``-a`` sets the volume (around 100) and ``-p``
sets the pitch (around 50). Sample announcement sounds are available
from `freesound <http://www.freesound.org/tagsViewSingle.php?id=1885>`__.

festival
^^^^^^^^

A ``cmdline`` speech tag can call a wrapper script that passes the
phrase and language to festival:

.. code-block:: xml

   <speech type="cmdline" data="/usr/local/bin/speech-wrapper %s spanish">

Save the following as ``/usr/local/bin/speech-wrapper`` and make it
executable:

.. code-block:: bash

   #!/bin/sh
   echo "$1" | festival --tts --language "$2"

Dutch street names can be pronounced more clearly with the Dutch
festival extensions from
`nextens <http://nextens.uvt.nl/nextens-wiki/DownloadNextens>`__.

flite
^^^^^

Mbrola
^^^^^^

With espeak-ng and a German mbrola voice:

.. code-block:: xml

   <speech type="cmdline"
           data="espeak-ng -s 170 -a 150 -p 40 -v mb-de7 '%s'"
           cps="15"/>

pico2wave
^^^^^^^^^

The ``pico2wave`` engine (also known as speech2wave) can be used through
a wrapper script:

.. code-block:: xml

   <speech type="cmdline" data="/home/mobian/bin/speech2text '%s'" cps="15"/>

.. code-block:: bash

   #!/bin/bash
   function cleanup() {
     rm -f /tmp/$$.wav
   }

   trap cleanup TERM EXIT
   pico2wave -l de-DE -w /tmp/$$.wav "$@"
   aplay /tmp/$$.wav

Install it with:

.. code-block:: bash

   sudo apt install libttspico-utils

piper-tts
^^^^^^^^^

Piper can be used through a similar wrapper:

.. code-block:: xml

   <speech type="cmdline" data="/home/mobian/bin/speech2text '%s'" cps="5"/>

.. code-block:: bash

   #!/bin/bash
   piper -m /home/mobian/tts/de_DE-thorsten-high.onnx \
     --output-raw <<< "$@" | aplay -r 22500 -c 1 -f S16_LE

Install it with:

.. code-block:: bash

   sudo apt install pipx
   pipx install piper-tts

Then obtain the voice files and configuration, for example from
`Thorsten Voice`__.

__ https://www.thorsten-voice.de/thorsten-voice-%f0%9f%92%9b-piper/

KDE text-to-speech
^^^^^^^^^^^^^^^^^^

Older KDE setups used kttsd:

.. code-block:: xml

   <speech type="cmdline" data="dcop kttsd KSpeech sayText %s de" cps="15"/>

On Debian Bookworm, mbrola packages are available in the repositories
and kttsd has been replaced by speech-dispatcher in modern KDE versions.
A working setup typically uses speech-dispatcher with an appropriate
voice; see the speech-dispatcher documentation for current instructions.

Online speech with Google Translate
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Navit directions can be sent to the Google Translate TTS API, which
returns an MP3 that a local player can play. This needs an internet
connection and sends the spoken directions to an online service. The
script below was generated and tested on Windows.

Install VLC portable and place ``vlc.exe`` in the Navit folder. Save the
following as a ``.bat`` file in the same folder:

.. code-block:: bat

   set ESCAPED=%~1%
   wget -q -U Mozilla -O output.mp3 ^
     "http://translate.google.com/translate_tts?ie=UTF-8&tl=en&q=%ESCAPED%"
   FULL-PATH-TO/vlc.exe --qt-start-minimized --play-and-exit -q output.mp3

Replace ``FULL-PATH-TO`` with the full path to ``vlc.exe``. Download
`WGET <http://users.ugent.be/~bpuype/wget/>`__ and put it next to the
batch file.

Point the Navit speech tag at the batch file:

.. code-block:: xml

   <speech type="cmdline" data="C:\path\to\navit\speech.bat %s" />

Change ``tl=en`` in the script to the required country tag, for example
``tl=nl`` for Dutch or ``tl=de`` for German.

Android
^^^^^^^

.. code-block:: xml

    <speech type="android" cps="15"/>

.. _start_up_in_silent_mode:

Start up in silent mode
^^^^^^^^^^^^^^^^^^^^^^^

To have Navit start up in silent mode, insert ``active="0"`` somewhere
in your ``speech`` tag. For example on Android:

.. code-block:: xml

    <speech type="android" cps="15" active="0"/>

In this case, you should place a ``toggle_announcer`` item in your
:doc:`/user/configuration/OSD` configuration, or add a menu item so you
can enable speech output when you need it.

Debugging
~~~~~~~~~

The ``debug`` tag sets the debug level for a particular Navit module:

.. code-block:: xml

    <debug name="gui_internal" dbg_level="info"/>

Attributes:

* **name** - the module name; optionally a function name in that module
  can be appended with a colon (e.g. ``navit:do_draw``). The module name
  is defined by the ``module_add_library()`` call in each plugin's
  ``CMakeLists.txt`` file. Common examples: ``navit``, ``gui_internal``,
  ``map_binfile``, ``speech_speech_dispatcher``, ``vehicle_gpsd``.
* **dbg_level** - the level to set for the component named by ``name``:
  ``error``, ``warning``, ``info`` or ``debug`` (default ``error``).
* **level** *(older numeric form)* - equivalent to ``dbg_level``, but
  using a number (0-3) instead of the level's name. Prefer ``dbg_level``.

Some special module names are available:

* Setting a level > 0 for **"timestamps"** enables printing of timestamps
  in debug messages.
* Setting **"segv"** to any value >= 1 installs a segmentation-fault
  handler that prints a backtrace. With a value of 1, gdb is run
  non-interactively just to capture the backtrace and then quit; with a
  value > 1, gdb is started and kept attached for interactive debugging.
* **"global"** sets the global debug level (applies to all modules); this
  is the same as using the command line option ``-d``.

Bookmarks
~~~~~~~~~

See `Add Bookmarks with Dbus <Dbus#add_bookmark_signal>`__

Navigation and routing
~~~~~~~~~~~~~~~~~~~~~~

Tracking
^^^^^^^^

The ``tracking`` tag controls how Navit follows the position reported by
the active vehicle while it is moving:

.. code-block:: xml

    <tracking cdf_histsize="4" route_pref="1000"/>

Attributes:

* **cdf_histsize** - the number of recent position samples kept for the
  cumulative-displacement (CDF) filter that smooths the reported
  position. A value of 0 disables the filter. See
  `this article <http://julien.cayzac.name/code/gps/>`__ for behind the
  filter.
* **route_pref** - the routing bonus (added to a street's routing value)
  that makes Navit prefer staying on the calculated route. Increasing
  this value helps to stay on track while receiving an inaccurate GPS
  position. The default is 300; use 1000 to 3000 if the device tends to
  skip from the track.

Announcements
^^^^^^^^^^^^^

Voice announcements are defined with ``announce`` tags inside a
``navigation`` tag:

.. code-block:: xml

    <navigation>
      <announce type="street_0,street_1_city" level0="300" level1="1000" level2="2000"/>
    </navigation>

Attributes:

* **type** - types of ways for which this announcement is valid.
* **level0** *(metres)* - the distance at which the final announcement
  is made (i.e. "turn left now").
* **level1** *(metres)* - the distance at which the intermediate
  announcement is made (i.e. "turn left in 1km").
* **level2** *(metres)* - the distance at which the first announcement
  is made (i.e. "turn left soon").

.. _splitting_navit.xml:

Splitting navit.xml
~~~~~~~~~~~~~~~~~~~

Navit has support for a small subset of **XInclude** / **XPath** for
including parts of external XML files. Supported is a tag like

.. code-block:: xml

    <xi:include href="some_file" xpointer="xpointer_stuff" />

You can leave out either href (xi:include refers to the same file it is
in then) or xpointer (xi:include then refers the complete file), but not
both. The *href* attribute refers to a file relative to the current
directory. It is suggested to use the complete path, such as
*/home/root/.navit/navit-vehicles.xml*.

href is expanded with wordexp internally, so you can do stuff like:

.. code-block:: xml

    <xi:include href="$NAVIT_SHAREDIR/maps/*.xml" />

Some examples on the supported syntax:

.. code-block:: xml

    <xi:include xpointer="xpointer(/config/navit/layout[@name='Car']/layer[@name='points'])" />

references to the XML-Tag "layer" with attribute "name" of value
"points" within an XML-Tag "layout" with attribute "name" of value "Car"
within an XML-Tag "navit" within an XML-Tag "config".

.. code-block:: xml

    <config xmlns:xi="http://www.w3.org/2001/XInclude">
    <xi:include href="$NAVIT_SHAREDIR/navit.xml" xpointer="xpointer(/config/*[name(.)!='navit'])"/>
    <navit center="4808 N 1134 E" zoom="256" tracking="1" cursor="1" orientation="0">
    <xi:include href="$NAVIT_SHAREDIR/navit.xml" xpointer="xpointer(/config/navit/*[name(.)!='vehicle'])"/>
    </navit>
    </config>

Use this as your $HOME/.navit/navit.xml and you will get everything
under .. except .. (first xi:include), plus as specified plus everything
from navit within config, except the vehicle definitions (second
xi:include).
