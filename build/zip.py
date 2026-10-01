#!/usr/bin/env python3
#
# Copyright 2014 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Archives a set of files."""

import argparse
import fnmatch
import json
import re
import sys
import zipfile

import action_helpers
import gn_helpers
import zip_helpers


def _expand_file_args(args):
  """Replaces @FileArg(filename:key...) placeholders in command arguments."""
  new_args = list(args)
  file_jsons = {}
  pattern = re.compile(r'@FileArg\((.*?)\)')
  for i, arg in enumerate(args):
    match = pattern.search(arg)
    if not match:
      continue

    def get_key(key):
      if key.endswith('[]'):
        return key[:-2], True
      return key, False

    lookup_path = match.group(1).split(':')
    file_path, _ = get_key(lookup_path[0])
    if file_path not in file_jsons:
      with open(file_path, encoding='utf-8') as f:
        file_jsons[file_path] = json.load(f)

    expansion = file_jsons
    for key in lookup_path:
      key, flatten = get_key(key)
      expansion = expansion[key]
      if flatten:
        if not isinstance(expansion, list) or len(expansion) != 1:
          raise ValueError(f'Expected single item list but got {expansion}')
        expansion = expansion[0]

    if isinstance(expansion, list):
      value = gn_helpers.ToGNString(expansion)
    else:
      value = str(expansion)
    new_args[i] = arg[:match.start()] + value + arg[match.end():]
  return new_args


def main(args):
  parser = argparse.ArgumentParser()
  parser.add_argument('--input-files', help='GN-list of files to zip.')
  parser.add_argument(
      '--input-files-base-dir',
      help='Paths in the archive will be relative to this directory')
  parser.add_argument('--input-zips', help='GN-list of zips to merge.')
  parser.add_argument(
      '--input-zips-excluded-globs',
      help='GN-list of globs for paths to exclude.')
  parser.add_argument('--output', required=True, help='Path to output archive.')
  compress_group = parser.add_mutually_exclusive_group()
  compress_group.add_argument(
      '--compress', action='store_true', help='Compress entries')
  compress_group.add_argument(
      '--no-compress',
      action='store_false',
      dest='compress',
      help='Do not compress entries')
  parser.add_argument('--comment-json',
                      action='append',
                      metavar='KEY=VALUE',
                      type=lambda x: x.split('=', 1),
                      help='Entry to store in JSON-encoded archive comment.')
  action_helpers.add_depfile_arg(parser)
  options = parser.parse_args(_expand_file_args(args))

  with action_helpers.atomic_output(options.output) as f:
    with zipfile.ZipFile(f.name, 'w') as out_zip:
      depfile_deps = None
      if options.input_files:
        files = action_helpers.parse_gn_list(options.input_files)
        zip_helpers.add_files_to_zip(
            files,
            out_zip,
            base_dir=options.input_files_base_dir,
            compress=options.compress)

      if options.input_zips:
        files = action_helpers.parse_gn_list(options.input_zips)
        depfile_deps = files
        path_transform = None
        if options.input_zips_excluded_globs:
          globs = action_helpers.parse_gn_list(
              options.input_zips_excluded_globs)
          path_transform = (
              lambda path: None
              if globs and any(fnmatch.fnmatch(path, glob) for glob in globs)
              else path)
        zip_helpers.merge_zips(
            out_zip,
            files,
            path_transform=path_transform,
            compress=options.compress)

      if options.comment_json:
        out_zip.comment = json.dumps(
            dict(options.comment_json), sort_keys=True).encode('utf-8')

  if options.depfile:
    action_helpers.write_depfile(
        options.depfile, options.output, inputs=depfile_deps)


if __name__ == '__main__':
  main(sys.argv[1:])
