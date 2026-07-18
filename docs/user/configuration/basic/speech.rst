.. _speech:

Speech Output
=============

Navit supports text-to-speech output for navigation instructions. This page describes how to configure speech output.


eSpeak
------

eSpeak is a free, open source speech synthesizer. It is the most commonly used speech engine for Navit.

Installation
^^^^^^^^^^^^

Install eSpeak via your package manager:

.. code-block:: bash

   # Debian/Ubuntu
   apt-get install espeak

   # Fedora
   dnf install espeak

   # Arch Linux
   pacman -S espeak


Configuration
^^^^^^^^^^^^^

Add the following to your ``navit.xml``:

.. code-block:: xml

   <speech type="cmdline" data="espeak -s 150 -v english %s" />

Parameters:

- ``-s``: Speed in words per minute (150 is a good default)
- ``-v``: Language/voice to use (e.g., english, german, french)
- ``%s``: Placeholder for the text to speak


Mbrola
------

Mbrola is a speech synthesizer that can be used with eSpeak for higher quality speech output.

Installation
^^^^^^^^^^^^

.. code-block:: bash

   # Install mbrola
   apt-get install mbrola

   # Install voice data (example for English)
   apt-get install mbrola-en1


Configuration
^^^^^^^^^^^^^

First, test the mbrola voice:

.. code-block:: bash

   espeak -v mb/mb-en1 "This is mbrola voice english 1"

Then configure in ``navit.xml``:

.. code-block:: xml

   <speech type="cmdline" data="espeak -v mb/mb-en1 -s 150 -a 150 -p 50 %s" />


Festival
--------

Festival is another speech synthesis system that can be used with Navit.

.. code-block:: xml

   <speech type="cmdline" data="/usr/local/bin/speech-wrapper %s english" />

Create ``/usr/local/bin/speech-wrapper``:

.. code-block:: bash

   #!/bin/sh
   echo "$1" | festival --tts --language "$2"

Don't forget to make it executable:

.. code-block:: bash

   chmod +x /usr/local/bin/speech-wrapper


Custom Scripts
--------------

You can create custom scripts for more complex speech output. For example, to play a sound before speaking:

Create ``/usr/local/bin/speech-wrapper``:

.. code-block:: bash

   #!/bin/bash
   aplay -r 44100 /path/to/sound.wav
   espeak -v mb-en1 -s 150 -a 150 -p 50 "$1"

Then use it in ``navit.xml``:

.. code-block:: xml

   <speech type="cmdline" data="/usr/local/bin/speech-wrapper %s" />


Language Support
----------------

eSpeak supports many languages. Use the ``-v`` parameter to specify the language:

- English: ``en`` or ``english``
- German: ``de`` or ``german``
- French: ``fr`` or ``french``
- Spanish: ``es`` or ``spanish``

For a complete list of supported languages, run:

.. code-block:: bash

   espeak --voices


Troubleshooting
---------------

Speech is not playing
^^^^^^^^^^^^^^^^^^^^^

- Check that eSpeak is installed: ``which espeak``
- Test directly: ``espeak "Hello world"``
- Check the speech command in ``navit.xml`` for typos

Audio device busy
^^^^^^^^^^^^^^^^^

If you get errors about ``/dev/dsp`` being busy, you can redirect audio to ALSA:

.. code-block:: bash

   #!/bin/sh
   espeak -s 150 -v "$2" --stdout "$1" | aplay > /dev/null


Links
-----

- `eSpeak homepage <http://espeak.sourceforge.net/>`_
- `Mbrola project <http://www.tcts.fpms.ac.be/synthesis/mbrola.html>`_
