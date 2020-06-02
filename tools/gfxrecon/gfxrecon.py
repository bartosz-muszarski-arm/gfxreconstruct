#!/usr/bin/env python3
#
# Copyright (c) 2020 LunarG, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#
# Utility for invoking gfxrecon commands
# Usage:
#
#     gfxrecon.py [capture|compress|extract|info|replay] [<args>]
#
#         args is a command-specific argument list

import argparse
import os
import sys
import subprocess

argv = sys.argv
argc = len(sys.argv)

# Supported commands
valid_commands = [
    'capture',
    'compress',
    'extract',
    'info',
    'replay'
]

def IsWindows():
   return (sys.platform == 'win32' or sys.platform == 'cygwin')

# Function to return an executable for the given command.
# Searches for an executable command in:
#    current directory
#    PATH
#    ../cmd  (on Linux only, so that this script works when run from a repo build)
#    ../cmd/*  (on Windows only, so that this script works when run from a repo build)
# If the above search fails, then it tries to find a python script named cmd.py in:
#    current directory
#    PATH
#    ../cmd
# If the a python script is found, the availability of a python interpreter is checked.
#
# On success, returns a list that can be passed to subprocess.run to execute the command,
# possibly including the python interpreter as the first element in the list.
#
# If an executable/python script is not found, or a python interpreter for a python script is
# not found, prints an error message and exits.

def GetExecutable(cmd):

    def IsExe(fpath):
        return os.path.isfile(fpath) and os.access(fpath, os.X_OK)

    # Set the command we are looking for
    if IsWindows():
        cmdexe = 'gfxrecon-' + cmd + '.exe'
    else:
        cmdexe = 'gfxrecon-' + cmd

    # Search for cmdexe in current dir
    if IsExe(cmdexe):
        return [os.path.join('.', cmdexe)]

    # Search for cmdexe in PATH
    for path in os.environ['PATH'].split(os.pathsep):
        c = os.path.join(path, cmdexe)
        if IsExe(c):
            return [c]

    # Windows: Search for cmdexe in <scriptdir>/../cmd/*
    # (<scriptdir> is the dir that this script is located in.)
    scriptdir = os.path.dirname(os.path.realpath(__file__))
    if IsWindows():
       for buildtype in ['Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel']:
            c = os.path.join(scriptdir, '..', cmd, buildtype, cmdexe)
            if IsExe(c):
                return [c]
    else:
        # Linux: Search for cmdexe in <scriptdir>/../cmd
        c = os.path.join(scriptdir, '..', cmd, cmdexe)
        if IsExe(c):
            return [c]

    cmdpy = 'gfxrecon-' + cmd + '.py'

    # Search for cmdpy in current dir
    if os.path.isfile(cmdpy):
        return [sys.executable, cmdpy]

    # Search for cmdpy in PATH
    for path in os.environ['PATH'].split(os.pathsep):
        c=os.path.join(path, cmdpy)
        if os.path.isfile(c):
            return [sys.executable, c]

    # Search for cmdpy <scriptdir>/../cmd
    c = os.path.join(scriptdir, '..', cmd, cmdpy)
    if os.path.isfile(c):
        return [sys.executable, c]

    # Didn't find the executable or py command, error out
    print('Error: Cannot find ' + cmdexe + ' or ' + cmdpy + ' to execute')
    sys.exit(1)

def CreateCommandParser():
    parser = argparse.ArgumentParser(description='GFXReconstruct utility launcher.')
    parser.add_argument('command', choices=valid_commands, metavar='command', help='Command to execute. Valid options are [{}]'.format(', '.join(valid_commands)))
    parser.add_argument('args', nargs=argparse.REMAINDER, help='Command-specific argument list. Specify -h after command name for command help.')
    return parser

if __name__ == '__main__':
    command_parser = CreateCommandParser()
    command = command_parser.parse_args()
    extras = sys.argv[2::]
    cmd = GetExecutable(command.command)
    result = subprocess.run(cmd + extras)
    sys.exit(result.returncode)