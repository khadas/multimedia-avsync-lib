#!/bin/bash

#version rule:MAJORVERSION.MINORVERSION.COMMIT_COUNT-g(COMMIT_ID)

BASE=$(pwd)
echo $BASE

#major version
MAJORVERSION=1

#minor version
MINORVERSION=3

#modue name/
MODULE_NAME=MM-module-name:avsync-lib

#get all commit count
COMMIT_COUNT=$(git rev-list HEAD --count)
echo commit count $COMMIT_COUNT

#get current commit id
COMMIT_ID=$(git rev-parse --short HEAD)
echo commit id $COMMIT_ID

#find the module name line
MODULE_NAME_LINE=`sed -n '/\"MM-module-name/=' src/aml_version.h`
#echo $VERSION_LINE

#version rule string
VERSION_STRING=${MAJORVERSION}.${MINORVERSION}.${COMMIT_COUNT}-g${COMMIT_ID}

#update the original version
if [ ${MODULE_NAME_LINE} -gt 0 ]; then
sed -i -e ${MODULE_NAME_LINE}s"/.*/\"${MODULE_NAME},version:${VERSION_STRING}\"\;/" src/aml_version.h
fi
