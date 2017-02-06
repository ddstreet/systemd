#!/bin/bash

# Don't use uuencode/uudecode: it's an additional build dependency
USE_UUENCODE=0

# arbitrary; any with more binary chars than this is uuencoded
BINARY_MAX=16

FILENAME="$1"
shift

## Functions

function is_printable_char()
{
    # TAB or LF
    if [[ $1 -eq 0x09 || $1 -eq 0x0a ]] ; then
        return 0
    fi

    # control chars
    if [[ $1 -le 0x1f || $1 -ge 0x7f ]] ; then
        return 1
    fi

    # the rest are printable chars
    return 0
}

function is_special_char()
{
    # single quote and backslash are special inside '' string
    [[ $1 -eq 0x27 || $1 -eq 0x5c ]]
}

function file_is_printable()
{
    hexdump -v -e '1/1 "%02x"' "$FILENAME" | while read -n 2 C
    do
        if ! is_printable_char 0x$C ; then
            return 1
        fi
    done
}

function file_is_mostly_printable()
{
    N=0
    hexdump -v -e '1/1 "%02x"' "$FILENAME" | while read -n 2 C
    do
        if ! is_printable_char 0x$C ; then
            N=$[ $N + 1 ]
            if [[ $N -gt $BINARY_MAX ]] ; then
                return 1
            fi
        fi
    done
}

function file_has_EOF()
{
    grep -q '^EOF$' "$FILENAME"
}

function file_has_final_LF()
{
    [[ $( sed -ne '$p' "$FILENAME" | wc -l ) -eq 1 ]]
}

function do_empty()
{
    echo "echo -ne '' > \"$FILENAME\""
}

function do_here_document()
{
    echo "cat << \"EOF\" > \"$FILENAME\""
    cat "$FILENAME"
    echo "EOF"
    if ! file_has_final_LF ; then
        echo "# remove final LF"
        echo "truncate -s -1 \"$FILENAME\""
    fi
}

# requires $SIZE to be set
function do_escaped_echo()
{
    S=0
    REDIR='>'
    THISLINE=""
    hexdump -v -e '1/1 "%02x"' "$FILENAME" | while read -n 2 C
    do
        S=$[ $S + 1 ]
        if [[ $S -gt $SIZE ]] ; then
            echo "# ERROR: got more bytes than expected ($SIZE)"
            break
        fi
        if [[ 0x$C -eq 0x0a ]] ; then
            echo -e "echo -e '$THISLINE' $REDIR \"$FILENAME\""
            THISLINE=""
            REDIR='>>'
            continue
        fi
        if is_printable_char 0x$C && ! is_special_char 0x$C ; then
            THISLINE+="\\x$C"
        else
            THISLINE+="\\\\x$C"
        fi
        if [[ $S -eq $SIZE ]] ; then
            echo -e "echo -ne '$THISLINE' $REDIR \"$FILENAME\""
            continue
        fi
    done
}

function do_uuencode()
{
    echo "uudecode << \"EOF\" > \"$FILENAME\""
    uuencode "$FILENAME" -
    echo "EOF"
}

function process_file()
{
    SIZE=$( stat -c %s "$FILENAME" )
    LINES=$( cat "$FILENAME" | wc -l )

    if [[ $SIZE -eq 0 ]] ; then
        do_empty
    elif [[ $LINES -ge 2 ]] && file_is_printable && ! file_has_EOF ; then
        do_here_document
    elif file_is_mostly_printable ; then
        do_escaped_echo
    elif [[ $USE_UUENCODE -eq 1 ]] ; then
        do_uuencode
    else # default to escaped echo
        do_escaped_echo
    fi
}

## Start processing

MODE=$( stat -c %#a "$FILENAME" )

# Must test for symlink first, as other tests dereference link
if [[ -L $FILENAME ]] ; then
    LINK="$( readlink "$FILENAME" )"
    echo "ln -s \"$LINK\" \"$FILENAME\""
elif [[ -d $FILENAME ]] ; then
    echo "# $FILENAME"
    echo "mkdir -m $MODE \"$FILENAME\""
elif [[ -f $FILENAME ]] ; then
    process_file
    echo "chmod $MODE \"$FILENAME\""
else
    TYPE=$( stat -c %F "$FILENAME" )
    echo "# ERROR: unknown entry type '$TYPE' for $FILENAME"
    exit 1
fi
