#!/bin/sh
# Turns an ASYNCIFY_ONLY list (one function per line, # comments, * wildcards
# as Binaryen matches them) into a --pre-js snippet that hands the same list
# to WEB_JS_CanSuspend (web/web_js.c).
printf "Module['asyncifyFunctions'] = ["
grep -v '^#' "$1" | grep -v '^[[:space:]]*$' | sed 's/.*/"&",/' | tr -d '\n'
printf "];\n"
