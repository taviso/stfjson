#define _XOPEN_SOURCE 500
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include <err.h>

#include <json.h>

//
// Quick code to convert Lotus Agenda STF format to JSON.
//
// Author: taviso@gmail.com
// Date: October, 2020
//

// How you want dates to appear in JSON output.
#define JSON_DATE_FORMAT "%Y-%m-%dT%H:%M:%SZ"

#define STF_OPEN_TAG '{'
#define STF_CLOSE_TAG '}'
#define STF_ESCAPE_TAG ' '

// The table from Appendix B-7, translated into strptime formats.
// NOTE: The manual incorrectly claims 2-digit years are used.
const char * kLotusDateFmt[] = {
    NULL,                // 0
    "%m/%d/%Y %H:%M",    // 1
    "%m/%d/%Y %H:%M",    // 2
    "%d.%m.%Y %H:%M",    // 3
    "%Y-%m-%d %H:%M",    // 4
    "%d-%b %H:%M",       // 5
    "%d-%b-%Y %H:%M",    // 6
    "%m/%d/%Y %I:%M%p",  // 7
    "%d/%m/%Y %I:%M%p",  // 8
    "%d.%m.%Y %I:%M%p",  // 9
    "%Y-%m-%d %I:%M%p",  // 10
    "%d-%b %I:%M%p",     // 11
    "%d-%b-%Y %I:%M%p",  // 12
};

// These are the tags from Appendix B-4
// Documented
// {d}      Specified a date format, such as MM/DD/YY
// {C}      Beginning of a category specification (the category and family with
//          any associated notes)
// {D}      Done date
// {F}      Beginning of a category note
// {E}      Entry date
// {G}      Name of the note file for the category
// {I}      Beginning of an item specification (the item and associated
//          categories, and notes)
// {N}      Beginning of an item note
// {O}      Name of the note file for an item
// {S}      Beginning of comment text to be ignored when imported
// {STF}    Header that begins a structured file
// {T}      Beginning of the text of an item
// {W}      When date
// {.}      End of a category specification
// {!}      End of an item specification

// Undocumented
// { ...    Escaped STF tag, remove the space then emit verbatim.
// {r}      Category attribute?
//          AC      - Apply Conditions?
//          PEA     - Protected?
// {;}      End of attribute/link.
// {p}      Category Assignment Conditions
// {a}      Category Assignment Action
// {+}      Category Include
// {-}      Category Exclude

// Category Type Symbols (Appendix B-11)
//  \       Standard category
//  /       Exclusive
//  |       Unindexed (Note: manual says ¦, but all samples use |)
//  #|      Numeric
//  @|      Date
//
// Note that as described in Appendix B-13, Agenda uses % as an escape
// character for literal symbols.

int read_stf_chunk(char **tag, char **value)
{
    size_t tagsz, valsz;
    size_t tagmax, valmax;
    int c;
    enum {
        STF_CHUNK_TAG,
        STF_CHUNK_DATA,
        STF_CHUNK_COMMENT,
        STF_CHUNK_NOTE,
        STF_CHUNK_END,
    } state;

    // Everything before the tag is a comment.
    state = STF_CHUNK_COMMENT;

    // Initialize everything to zero.
    tagsz   = 0;
    valsz   = 0;
    *tag    = 0;
    *value  = 0;
    tagmax  = 0;
    valmax  = 0;

    while (state != STF_CHUNK_END) {
        // Read next character of input.
        if ((c = getc(stdin)) == EOF)
            break;

        switch (state) {
            // If anything appears before a tag, then it is a comment.
            case STF_CHUNK_COMMENT:

                // Just ignore any leading whitespace.
                if (isspace(c))
                    continue;

                // OK, a tag is being opened, start reading it.
                if (c == STF_OPEN_TAG) {
                    state = STF_CHUNK_TAG;
                    break;
                }

                // OK, this comment has actual content, fake a comment tag.
                state = STF_CHUNK_DATA;

                tagsz   = 1;
                tagmax  = 1;
                *tag    = strdup("S");

                // fallthrough
            case STF_CHUNK_DATA:

                // Check if this is the start of a new tag, therefore the end
                // of our data.
                if (c == STF_OPEN_TAG) {
                    int tagc = getc(stdin);

                    // If the first character was an escape, this is not a tag.
                    if (tagc != STF_ESCAPE_TAG) {
                        ungetc(tagc, stdin);
                        ungetc(c, stdin);

                        // finished
                        state = STF_CHUNK_END;

                        // Trim any trailing whitespace.
                        while (valsz && isspace((*value)[valsz - 1]))
                            (*value)[--valsz] = '\0';
                        break;
                    }
                }

                // Discard leading whitespace.
                if (isspace(c) && !valsz)
                    break;

                // Grow buffer if necessary.
                if (valsz >= valmax) {
                    *value = realloc(*value, valmax += 1024);

                    // Initialize to zero.
                    memset(*value + valsz, 0, valmax - valsz);
                }

                (*value)[valsz++] = c;
                break;
            case STF_CHUNK_TAG:
                // Check if we've finished reading the tagname.
                if (c == STF_CLOSE_TAG) {
                    state = STF_CHUNK_DATA;

                    if (!tagsz) {
                        warnx("found an empty tag, data maybe malformed");
                        break;
                    }

                    // There are some tags that don't have data, just end.
                    if (strcmp(*tag, ";") == 0    // UNDOCUMENTED; End of attribute.
                     || strcmp(*tag, "+") == 0    // UNDOCUMENTED; Category relationship.
                     || strcmp(*tag, "-") == 0    // UNDOCUMENTED; Category relationship.
                     || strcmp(*tag, ".") == 0    // End of a category specification.
                     || strcmp(*tag, "!") == 0) { // End of an item specification.
                        state = STF_CHUNK_END;
                    }
                    break;
                }
                if (tagsz >= tagmax) {
                    *tag = realloc(*tag, tagmax += 32);

                    // Initialize to zero.
                    memset(*tag + tagsz, 0, tagmax - tagsz);
                }

                (*tag)[tagsz++] = c;
                break;
        }
    }

    if (state != STF_CHUNK_END)
        return -1;

    //fprintf(stderr, "read a {%s} tag with data %s\n", *tag, *value);
    return 0;
}

void parse_item_category(struct json_object *links, int dateformat, const char *def)
{
    char *token;
    char *names;
    char *value;
    char *root;
    size_t length;
    struct json_object *link;
    enum {
        STF_CAT_STANDARD,
        STF_CAT_EXCLUSIVE,
        STF_CAT_DATE,
        STF_CAT_UNINDEXED,
        STF_CAT_NUMERIC,
    } type;

    length = strlen(def);
    names  = NULL;
    value  = NULL;
    root   = NULL;

    // Must be at least two characters, one char name and one char type.
    if (length < 2) {
        errx(EXIT_FAILURE, "attempted to parse invalid category link");
    }

    link = json_object_new_object();

    // First determine what kind of definition this is.
    // If the last character is \, then this is a standard entry with no data.
    if (def[length-1] == '\\' && def[length-2] != '%') {
        names = strndup(def, length - 1);
        type  = STF_CAT_STANDARD;
        json_object_object_add(link, "type", json_object_new_string("standard"));
        goto parsenames;
    }

    // Same as above, but this is an exclusive category.
    if (def[length-1] == '/' && def[length-2] != '%') {
        names = strndup(def, length - 1);
        type = STF_CAT_EXCLUSIVE;
        json_object_object_add(link, "type", json_object_new_string("exclusive"));
        goto parsenames;
    }

    // Unindexed, but need to check if it's numeric or date.
    if (def[length-1] == '|'
            && def[length-2] != '%'
            && def[length-2] != '@'
            && def[length-2] != '#') {
        names = strndup(def, length - 1);
        type = STF_CAT_UNINDEXED;
        json_object_object_add(link, "type", json_object_new_string("unindexed"));
        goto parsenames;
    }

    // I don't need to check for escape characters here, because if it's not a
    // real value, the pipe would be escaped.
    if ((value = strstr(def, "@|"))) {
        names = strndup(def, value - def);
        type  = STF_CAT_DATE;
        json_object_object_add(link, "type", json_object_new_string("date"));
        value += 2;
        goto parsenames;
    }

    if ((value = strstr(def, "#|"))) {
        names = strndup(def, value - def);
        type  = STF_CAT_NUMERIC;
        json_object_object_add(link, "type", json_object_new_string("numeric"));
        value += 2;
        goto parsenames;
    }

    errx(EXIT_FAILURE, "could not determine type of link %s", def);

parsenames:

    // Each link is an array element like {name: "Date", type: "", value: "12/12/123" }
    //fprintf(stderr, "parsing category %s, names=%s, value=%s\n", def, names, value);

    if ((token = strtok(names, ";")) == NULL) {
        errx(EXIT_FAILURE, "A category must have a name");
    }

    json_object_object_add(link, "name", json_object_new_string(token));

    if ((token = strtok(NULL, ";")) != NULL) {
        json_object_object_add(link, "shortname", json_object_new_string(token));
    }

    if ((token = strtok(NULL, ";")) != NULL) {
        struct json_object *alsomatch = json_object_new_array();
        do {
            json_object_array_add(alsomatch, json_object_new_string(token));
        } while ((token = strtok(NULL, ";")));

        json_object_object_add(link, "alsomatch", alsomatch);
    }

    if (value) {
        char *unescaped = strdupa(value);
        char timestamp[128];
        struct tm parsed = {0};

        // First remove all the escaped chars.
        for (char *p = unescaped; *p = *value++;) {
            if (*p != '%')
                p++;
            if (*p == ';')
                unescaped = p + 1;
        }
        switch (type) {
            case STF_CAT_DATE:
                // Parse the date with the current format.
                strptime(unescaped, kLotusDateFmt[dateformat], &parsed);
                if (strftime(timestamp, sizeof timestamp, JSON_DATE_FORMAT, &parsed) == 0) {
                    errx(EXIT_FAILURE, "failed to format timestamp for JSON");
                }
                // fprintf(stderr, "DATE %s => %s\n", unescaped, timestamp);
                json_object_object_add(link, "value", json_object_new_string(timestamp));
                break;
            default:
                errx(EXIT_FAILURE, "didn't expect this type to have a value");
        }
    }

    json_object_array_add(links, link);
    free(names);
    return;
}

enum {
    STF_STATE_NONE,
    STF_STATE_ROOT,
    STF_STATE_CATEGORY,
    STF_STATE_CATEGORY_COND,
    STF_STATE_CATEGORY_ACTIONS,
    STF_STATE_ITEM,
    STF_STATE_NOTE,
};

int main(int argc, char **argv)
{
    int state;
    int dateformat;
    char *tag, *value;

    struct json_object *root;
    struct json_object *stf;
    struct json_object *items;
    struct json_object *categories;
    struct json_object *itemcats;
    struct json_object *category;
    struct json_object *item;
    struct json_object *attributes;
    struct json_object *assignopts;
    struct json_object *include;
    struct json_object *exclude;

    root = json_object_new_array();
    state = STF_STATE_NONE;

    // The default dateformat is 1, Appendix B-6
    dateformat = 1;

    while (read_stf_chunk(&tag, &value) != -1) {
        // Just print comments to stderr.

        if (strcmp(tag, "S") == 0) {
            if (value) {
                fprintf(stderr, "Comment: %s\n", value);
            }
            free(tag);
            free(value);
            continue;
        }

      reparse:

        switch (state) {
            case STF_STATE_NONE:
                if (strcmp(tag, "STF") == 0) {
                    char timestamp[128];
                    struct tm date;

                    state = STF_STATE_ROOT;
                    stf = json_object_new_object();

                    // Appendix B-5
                    if (strptime(value, "%D;%T;002", &date) == NULL) {
                        errx(EXIT_FAILURE, "failed to parse STF header tag, '%s'", value);
                    }

                    if (strftime(timestamp, sizeof timestamp, JSON_DATE_FORMAT, &date) == 0) {
                        errx(EXIT_FAILURE, "failed to format timestamp for JSON");
                    }

                    json_object_object_add(stf, "timestamp", json_object_new_string(timestamp));
                    categories = json_object_new_array();
                    items = json_object_new_array();
                    json_object_object_add(stf, "categories", categories);
                    json_object_object_add(stf, "items", items);
                    json_object_array_add(root, stf);
                    break;
                }

                errx(EXIT_FAILURE, "[none] unexpected tag %s here", tag);
                break;
            case STF_STATE_ROOT:
                // Change date format, Appendix B-6
                if (strcmp(tag, "d") == 0) {
                    dateformat = strtoul(value, NULL, 10);

                    if (dateformat < 1 || dateformat > 12)
                        errx(EXIT_FAILURE, "invalid date format requested");

                    break;
                }

                // Start a new category definition.
                if (strcmp(tag, "C") == 0) {
                    state = STF_STATE_CATEGORY;
                    category = json_object_new_object();
                    attributes = json_object_new_array();

                    // The category name has symbols declaring it's type, see
                    // Appendix B-11.
                    // TODO: parse name.
                    json_object_object_add(category, "name", json_object_new_string(value));
                    json_object_object_add(category, "attributes", attributes);
                    json_object_array_add(categories, category);
                    break;
                }

                // Start a new item definition
                if (strcmp(tag, "I") == 0) {
                    state = STF_STATE_ITEM;
                    item = json_object_new_object();
                    itemcats = json_object_new_array();
                    json_object_object_add(item, "categories", itemcats);
                    json_object_array_add(items, item);
                    break;
                }

                // End of current file, new one begins.
                if (strcmp(tag, "STF") == 0) {
                    state = STF_STATE_NONE;
                    goto reparse;
                }

                errx(EXIT_FAILURE, "[root] unexpected tag %s here", tag);
                break;
            case STF_STATE_CATEGORY:
                // Undocumented, but Agenda 2.0b will generate these.
                if (strcmp(tag, "r") == 0) {
                    char *attrtag;
                    char *attrval;

                    json_object_array_add(attributes, json_object_new_string(value));

                    if (read_stf_chunk(&attrtag, &attrval) == -1) {
                        errx(EXIT_FAILURE, "failed to find end-attribute tag");
                    }

                    if (strcmp(attrtag, ";") != 0 || attrval != NULL) {
                        errx(EXIT_FAILURE, "invalid end-attribute tag");
                    }

                    free(attrtag);
                    free(attrval);
                    break;
                }

                // End of category.
                if (strcmp(tag, ".") == 0) {
                    category    = NULL;
                    attributes  = NULL;
                    state       = STF_STATE_ROOT;
                    break;
                }

                // Category note.
                if (strcmp(tag, "F") == 0) {
                    json_object_object_add(category, "note", json_object_new_string(value));
                    break;
                }

                // Undocumented tags.
                if (strcmp(tag, "p") == 0 || strcmp(tag, "a") == 0) {
                    assignopts = json_object_new_object();
                    include    = json_object_new_array();
                    exclude    = json_object_new_array();
                    json_object_object_add(assignopts, "include", include);
                    json_object_object_add(assignopts, "exclude", exclude);

                    if (strcmp(tag, "a") == 0) {
                        state = STF_STATE_CATEGORY_ACTIONS;
                        json_object_object_add(category, "actions", assignopts);
                    } else {
                        state = STF_STATE_CATEGORY_COND;
                        json_object_object_add(category, "conditions", assignopts);
                    }
                    break;
                }

                errx(EXIT_FAILURE, "[category] unexpected tag %s here", tag);
                break;
            case STF_STATE_CATEGORY_ACTIONS:
            case STF_STATE_CATEGORY_COND:
                if (strcmp(tag, "C") == 0) {
                    char *condtag;
                    char *condval;
                    if (read_stf_chunk(&condtag, &condval) == -1) {
                        errx(EXIT_FAILURE, "failed to find end-category tag");
                    }
                    if (strcmp(condtag, "+") == 0) {
                        json_object_array_add(include, json_object_new_string(value));
                    } else if (strcmp(condtag, "-") == 0) {
                        json_object_array_add(exclude, json_object_new_string(value));
                    } else {
                        errx(EXIT_FAILURE, "failed to find assignment type");
                    }
                    free(condtag);
                    free(condval);
                    break;
                }
                if (strcmp(tag, ";") == 0) {
                    state = STF_STATE_CATEGORY;
                    assignopts = NULL;
                    include    = NULL;
                    exclude    = NULL;
                    break;
                }
                errx(EXIT_FAILURE, "[categoryopts] unexpected tag %s here", tag);
            case STF_STATE_ITEM:
                if (strcmp(tag, "T") == 0) {
                    json_object_object_add(item, "text", json_object_new_string(value));
                    break;
                }
                if (strcmp(tag, "N") == 0) {
                    json_object_object_add(item, "note", json_object_new_string(value));
                    break;
                }
                // Any associated category
                if (strcmp(tag, "C") == 0) {
                    parse_item_category(itemcats, dateformat, value);
                    break;
                }
                if (strcmp(tag, ".") == 0) {
                    break;
                }
                if (strcmp(tag, "!") == 0) {
                    state = STF_STATE_ROOT;
                    item = NULL;
                    itemcats = NULL;
                    break;
                }
                errx(EXIT_FAILURE, "[item] unexpected tag %s here", tag);
            default:
                errx(EXIT_FAILURE, "unexpected state transition, %s", tag);
        }

        free(tag);
        free(value);
    }

    printf("%s\n", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
    json_object_put(root);
    return 0;
}
