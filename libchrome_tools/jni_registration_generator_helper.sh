#!/bin/bash
#
# Copyright (C) 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Generates jni.

set -e

args=()
files=()

jni_generator=''

for arg in "$@"; do
  case "${arg}" in
    --jni_generator=*)
      jni_generator=${arg#'--jni_generator='}
      ;;
    --*)
      args=("${args[@]}" "${arg}")
      ;;
    *)
      files=("${files[@]}" "${arg}")
      ;;
  esac
done

"${jni_generator}" --sources_files=<(printf "%q\n" "${files[@]}") "${args[@]}"
