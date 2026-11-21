#!/bin/sh
set -eu

./gambitree --help | grep -Fq 'cburnett'
./gambitree --help | grep -Fq -- '--fullscreen'
./gambitree --help | grep -Fq -- '--iconic'
./gambitree --fullscreen --iconic --help | grep -Fq 'Usage:'
./gambitree --title --fullscreen --iconic --help | grep -Fq 'Usage:'
./gambitree --help | grep -Fq -- '--role NAME'
./gambitree --role 'Study window' --help | grep -Fq 'Usage:'
./gambitree --title --role analysis --help | grep -Fq 'Usage:'
for role_case in missing empty; do
    status=0
    if [ "$role_case" = missing ]; then
        message=$(./gambitree --role 2>&1) || status=$?
    else
        message=$(./gambitree --role "" 2>&1) || status=$?
    fi
    [ "$status" -eq 2 ]
    printf '%s\n' "$message" | grep -Fq -- '--role requires a non-empty name'
done
./gambitree --help | grep -Fq -- '--board BOARD'
./gambitree --help | grep -Fq -- '--geometry GEO'
./gambitree --geometry 1200x800-0-0 --board wood --help | grep -Fq 'Usage:'
for geometry in '' bad 0x0 999999999999999999999999x99999999999999999999; do
    message=$(./gambitree --geometry "$geometry" --help 2>&1)
    printf '%s\n' "$message" | grep -Fq 'Usage:'
    printf '%s\n' "$message" | grep -Fq 'warning:'
done
message=$(./gambitree --geometry --help 2>&1)
printf '%s\n' "$message" | grep -Fq 'Usage:'
printf '%s\n' "$message" | grep -Fq 'warning:'
for board in classic wood wood-veneer; do
    ./gambitree --board "$board" --pieces cburnett --help | grep -Fq 'Usage:'
done
for board_case in missing empty unknown; do
    status=0
    if [ "$board_case" = missing ]; then
        message=$(./gambitree --board 2>&1) || status=$?
    elif [ "$board_case" = empty ]; then
        message=$(./gambitree --board "" 2>&1) || status=$?
    else
        message=$(./gambitree --board unknown 2>&1) || status=$?
    fi
    [ "$status" -eq 2 ]
    printf '%s\n' "$message" | grep -Fq -- '--board'
done
if ./gambitree --help | grep -Fq -- '--piece-theme'; then
    echo 'help still advertises the removed --piece-theme option' >&2
    exit 1
fi
./gambitree --pieces classic --help | grep -Fq 'Usage:'
./gambitree --pieces local_set-2 --help | grep -Fq 'Usage:'
status=0
message=$(./gambitree --piece-theme classic 2>&1) || status=$?
[ "$status" -eq 2 ]
printf '%s\n' "$message" | grep -Fq "unrecognized option '--piece-theme'"
./gambitree --help | grep -Fq -- '--title '
./gambitree --title "" --help | grep -Fq 'Usage:'
./gambitree --title --help | grep -Fq 'Usage:'
./gambitree --title --version | grep -Fq 'gambitree '
./gambitree --title Old --title --help | grep -Fq 'Usage:'
for option in --unknown --engine; do
    status=0
    message=$(./gambitree --title "$option" 2>&1) || status=$?
    [ "$status" -eq 2 ]
    if [ "$option" = --unknown ]; then
        printf '%s\n' "$message" | grep -Fq "unrecognized option '--unknown'"
    else
        printf '%s\n' "$message" | grep -Fq -- '--engine requires a path'
    fi
done

if ! command -v groff >/dev/null 2>&1 || ! command -v col >/dev/null 2>&1; then
    exit 0
fi

if LC_ALL=C tr -d '\011\012\015' < doc/gambitree.1 | LC_ALL=C grep '[[:cntrl:]]' >/dev/null 2>&1; then
    echo "manpage source contains an unexpected control character" >&2
    exit 1
fi

rendered=$(groff -Tascii -man -P-c doc/gambitree.1 | col -bx)
if printf '%s\n' "$rendered" | LC_ALL=C grep '[[:cntrl:]]' >/dev/null 2>&1; then
    echo "rendered manpage contains an unexpected control character" >&2
    exit 1
fi

# Compare words independently of groff's line wrapping and justification.
searchable=$(printf '%s\n' "$rendered" | tr -s '[:space:]' ' ')
printf '%s\n' "$searchable" | grep -Fq -- '--fullscreen'
printf '%s\n' "$searchable" | grep -Fq -- '--iconic'
printf '%s\n' "$searchable" | grep -Fq -- '--role NAME'
printf '%s\n' "$searchable" | grep -Fq -- '--geometry GEOMETRY'
printf '%s\n' "$searchable" | grep -Fq -- '--board BOARD'
if printf '%s\n' "$searchable" | grep -Fq -- '--piece-theme'; then
    echo 'manpage still advertises the removed --piece-theme option' >&2
    exit 1
fi
printf '%s\n' "$searchable" | grep -Fq 'names of locally installed piece folders may also be used'
printf '%s\n' "$searchable" | grep -Eq -- '--title (\[TITLE\]|TITLE)'
printf '%s\n' "$searchable" | grep -Fq 'Gambitree title, filename and available game index are shown'
printf '%s\n' "$searchable" | grep -Fq 'Cycle through Classic, Wood and Wood Veneer boards'
printf '%s\n' "$searchable" | grep -Fq 'Select the next alternative move offered at the same point'
printf '%s\n' "$searchable" | grep -Fq 'If there is no next choice inside an alternative line'
printf '%s\n' "$searchable" | grep -Fq 'Switch between piece letters and chess figurines'
printf '%s\n' "$searchable" | grep -Fq 'Gambitree remembers your chosen proportions'
printf '%s\n' "$searchable" | grep -Fq 'Make the move text larger or smaller'
printf '%s\n' "$searchable" | grep -Fq 'engine analysis'

printf '%s\n' "$searchable" | grep -Fq 'Show or hide a quick guide'
printf '%s\n' "$searchable" | grep -Fq 'Escape also closes the guide'
printf '%s\n' "$searchable" | grep -Fq 'Open the notation search bar'
printf '%s\n' "$searchable" | grep -Fq 'Press Enter or F3 for the next match'
printf '%s\n' "$searchable" | grep -Fq 'gambitree, meridian-shaded or cburnett'
