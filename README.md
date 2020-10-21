# stfjson

This code will attempt to parse the STF format that Lotus Agenda can export to,
and convert it into modern JSON. Lotus Agenda is an old PIM for MS-DOS, it's
unlikely anyone will ever use this code except for me 😂

The intention is to allow you to import Lotus Agenda data into other tools, and
a modern format like JSON has the tools to allow that.

The STF format is (partially) documented in the Lotus manual, "Working with
Definition Files", but a lot of the format is undocumented.

# Building

You need `libjson-c`, then just type `make`

# Usage

## Exporting Agenda Data to STF

In Lotus Agenda, you can either export an agenda file to STD manually:

```
F10 (menu) -> File -> Transfer -> Export
```

Or you can use automatic actions, so that when a new item is assigned to a
category it is automatically exported.

```
F9 (Category manager) -> F6 (Properties) -> Special actions -> Export item
```

## Parsing STF

Now that you have an STF file, you can use a tool like `jq` to query it, and
then import the data to something else.

- Print a list of all items
`$ ./stfjson < transfer.stf | jq '.[].items[].text'`

- Show all items with a due date
`$ ./stfjson < transfer.stf | jq '.[].items[] | select(.categories[].name=="\\When")'`

- Show all items with a due date in the future
`./stfjson < transfer.stf | jq '.[].items[] | { text: .text, due: (.categories[] | select(.name=="\\When") | .value | fromdate) } | select(.due > now)'`

Once you've extracted the data you need from jq, you can pipe it into another
application, like TaskWarrior, todo.sh, mailx, or whatever else.

# Bugs

In the highly unlikely event another human uses this, and you find it doesn't
parse an STF file created by Lotus Agenda correctly, please create an issue.
