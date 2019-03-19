#include "../gcode_parser.h"
#include "../gcode_interpreter.h"

#include <stdio.h>

typedef struct CLI {
    FILE* input;
    GCodeParser* parser;
    GCodeInterpreter* interp;
} CLI;

bool error(void* context, const char* text) {
    printf("*** ERROR: %s\n", text);
}

bool statement(void* context, GCodeStatementNode* statement) {
    gcode_interp_exec(((CLI*)context)->interp, statement);
    gcode_node_delete((GCodeNode*)statement);
    return true;
}

bool lookup(void* context, const GCodeVal* key, dict_handle_t parent,
            GCodeVal* result)
{
    // TODO
    return true;
}

const char* serialize(void* context, dict_handle_t dict) {
    // TODO
    return "";
}

bool exec(void* context, const char** fields, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (i == count - 1)
            printf("%s\n", fields[i]);
        else
            printf("%s ", fields[i]);
    return true;
}

void cli_delete(CLI* cli) {
    if (cli->input)
        fclose(cli->input);
    gcode_parser_delete(cli->parser);
    gcode_interp_delete(cli->interp);
    free(cli);
}

CLI* cli_new(const char* input_filename) {
    CLI* cli = malloc(sizeof(CLI));
    if (!cli) {
        fprintf(stderr, "Out of memory allocating CLI");
        return NULL;
    }
    memset(cli, 0, sizeof(CLI));

    cli->input = fopen(input_filename, "r");
    if (!cli->input) {
        error(cli, "Error opening input file");
        cli_delete(cli);
        return NULL;
    }

    cli->parser = gcode_parser_new(cli, error, statement);
    if (!cli->parser) {
        cli_delete(cli);
        return NULL;
    }

    cli->interp = gcode_interp_new(cli, error, lookup, serialize, exec);
    if (!cli->interp) {
        cli_delete(cli);
        return NULL;
    }

    return cli;
}

void cli_run(CLI* cli) {
    char buf[4096];

    while(!feof(cli->input)) {
        size_t length = fread(buf, 1, 4096, cli->input);
        if (ferror(cli->input)) {
            error(NULL, "I/O error reading input");
            return;
        }
        gcode_parser_parse(cli->parser, buf, length);
    }

    gcode_parser_finish(cli->parser);
}

int main(int argc, const char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s FILENAME\n", argv[0]);
        return 1;
    }

    CLI* cli = cli_new(argv[1]);
    if (!cli)
        return 1;

    cli_run(cli);

    cli_delete(cli);

    return 0;
}
