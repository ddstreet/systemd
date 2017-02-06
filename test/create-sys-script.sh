#!/bin/bash
#
# Use this only to (re-)create the test/sys-script.sh script,
# after adding or modifying anything in the test/sys/ directory
#

cd $( dirname "$0" )

if [[ ! -d sys ]] ; then
    echo "No sys/ directory; please create the sys/ dir before running this"
    exit 1
fi

if [[ -e sys-orig ]] ; then
    echo "Directory sys-orig exists; please restore to sys or remove it"
    exit 1
fi

OUTFILE=sys-script.sh

echo "Creating $OUTFILE using current contents of sys/"

cat <<"EOF" > $OUTFILE
#!/bin/bash

cd $( dirname "$0" )

EOF
chmod 755 $OUTFILE

find sys -fprintf /dev/stderr "%P                \r" \
     -exec ./create-sys-process-file.sh "{}" \; >> $OUTFILE

# move past final \r from find
echo ""

echo "Moving sys/ to sys-orig/"

mv -iv sys sys-orig

echo "Recreating sys from $OUTFILE"

./$OUTFILE

for n in sys sys-orig ; do
    pushd $n
    echo "Creating comparison list for $n"
    find -type d -fprintf /dev/stderr "%p                \r" \
         -printf "%#m %p\n" > ../$n.txt
    find -type l -fprintf /dev/stderr "%p                \r" \
         -printf "%P %l\n" >> ../$n.txt
    find -type f -fprintf /dev/stderr "%p                \r" \
         -printf "%#m %p\n" -exec shasum '{}' \; >> ../$n.txt
    echo ""
    popd
done

echo "Comparing sys-orig to sys"

diff -u sys-orig.txt sys.txt

