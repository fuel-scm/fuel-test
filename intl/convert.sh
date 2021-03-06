#!/bin/sh
SCRIPTDIR="$( cd "$( dirname "$0" )" && pwd )"
PRJDIR=$SCRIPTDIR/..
INTLDIR=$SCRIPTDIR

# Detect lrelease tool
if which lrelease-qt5 2>/dev/null; then
	LRELEASE="lrelease-qt5"
elif which lrelease4 2>/dev/null; then
	LRELEASE="lrelease4"
elif which lrelease 2>/dev/null; then
	LRELEASE="lrelease"
else
	echo "lrelease not found"
	exit 1
fi
	
echo "Using ${LRELEASE}"

echo "Converting localizations"

rm -rf $PRJDIR/rsrc/intl
mkdir $PRJDIR/rsrc/intl

for i in $INTLDIR/*.ts
do
	BASE=`basename $i .ts`

	# Convert all except the en_US which is
	# the original text in the code
	if [ "$BASE" != "en_US" ]; then
		echo "$TARGET"
		$LRELEASE $i -qm $PRJDIR/rsrc/intl/$BASE.qm
	fi
done

