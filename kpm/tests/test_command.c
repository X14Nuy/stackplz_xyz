#include <stdint.h>
#include <string.h>

#include "stackplz/core.h"
#include "test.h"

static int parse_text(const char *text, struct spz_command *out)
{
    char buffer[SPZ_MAX_COMMAND + 1U];
    size_t length = strlen(text);

    SPZ_EXPECT(length <= SPZ_MAX_COMMAND);
    if (length > SPZ_MAX_COMMAND)
        return -1;
    memcpy(buffer, text, length);
    return spz_parse_command(buffer, length, out);
}

static void expect_simple_commands(void)
{
    static const struct {
        const char *text;
        enum spz_command_kind kind;
    } cases[] = {
        {"status", SPZ_COMMAND_STATUS},
        {"profile", SPZ_COMMAND_PROFILE},
        {"clear", SPZ_COMMAND_CLEAR},
        {"audit", SPZ_COMMAND_AUDIT},
        {"poll", SPZ_COMMAND_POLL},
        {"poll after=42", SPZ_COMMAND_POLL},
        {"enable id=9", SPZ_COMMAND_ENABLE},
        {"disable id=10", SPZ_COMMAND_DISABLE},
        {"maps", SPZ_COMMAND_MAPS},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        struct spz_command command;
        int result = parse_text(cases[index].text, &command);

        SPZ_EXPECT_EQ(result, 0);
        SPZ_EXPECT_EQ(command.kind, cases[index].kind);
    }
}

static void expect_maps_read_commands(void)
{
    struct spz_command command;

    SPZ_EXPECT_EQ(parse_text("maps-read snapshot=7 offset=1536", &command), 0);
    SPZ_EXPECT_EQ(command.kind, SPZ_COMMAND_MAPS_READ);
    SPZ_EXPECT_EQ(command.value.maps_read.snapshot, UINT64_C(7));
    SPZ_EXPECT_EQ(command.value.maps_read.offset, UINT32_C(1536));

    SPZ_EXPECT_EQ(parse_text("maps-read offset=0 snapshot=18446744073709551615", &command), 0);
    SPZ_EXPECT_EQ(command.value.maps_read.snapshot, UINT64_MAX);
    SPZ_EXPECT_EQ(command.value.maps_read.offset, 0U);
}

static void expect_bind_commands(void)
{
    struct spz_command command;

    SPZ_EXPECT_EQ(parse_text("bind pid=31337 mode=pid uid=10234 comm=target.proc start=99887766", &command), 0);
    SPZ_EXPECT_EQ(command.kind, SPZ_COMMAND_BIND);
    SPZ_EXPECT_EQ(command.value.bind.pid, 31337U);
    SPZ_EXPECT_EQ(command.value.bind.mode, SPZ_BIND_PID);
    SPZ_EXPECT_EQ(command.value.bind.uid, 10234U);
    SPZ_EXPECT_EQ(command.value.bind.start_boot_time, UINT64_C(99887766));
    SPZ_EXPECT(command.value.bind.has_uid != 0U);
    SPZ_EXPECT(command.value.bind.has_comm != 0U);
    SPZ_EXPECT(command.value.bind.has_start_boot_time != 0U);
    SPZ_EXPECT(strcmp(command.value.bind.comm, "target.proc") == 0);

    SPZ_EXPECT_EQ(parse_text("bind mode=either pid=7", &command), 0);
    SPZ_EXPECT_EQ(command.value.bind.mode, SPZ_BIND_EITHER);
    SPZ_EXPECT(command.value.bind.has_uid == 0U);
    SPZ_EXPECT(command.value.bind.has_comm == 0U);
    SPZ_EXPECT(command.value.bind.has_start_boot_time == 0U);

    SPZ_EXPECT_EQ(parse_text("bind pid=8 mode=tgid", &command), 0);
    SPZ_EXPECT_EQ(command.value.bind.mode, SPZ_BIND_TGID);
}

static void expect_break_commands(void)
{
    static const struct {
        const char *text;
        enum spz_break_kind kind;
        enum spz_break_mode mode;
        uint8_t length;
    } cases[] = {
        {"break id=1 kind=x addr=0x1000 len=4 mode=once", SPZ_BREAK_EXECUTE, SPZ_BREAK_ONCE, 4U},
        {"break id=2 kind=r addr=0x1003 len=1 mode=repeat", SPZ_BREAK_READ, SPZ_BREAK_REPEAT, 1U},
        {"break id=3 kind=w addr=0x1004 len=4 mode=once", SPZ_BREAK_WRITE, SPZ_BREAK_ONCE, 4U},
        {"break id=4 kind=rw addr=0x1000 len=8 mode=repeat", SPZ_BREAK_READ_WRITE, SPZ_BREAK_REPEAT, 8U},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        struct spz_command command;

        SPZ_EXPECT_EQ(parse_text(cases[index].text, &command), 0);
        SPZ_EXPECT_EQ(command.kind, SPZ_COMMAND_BREAK);
        SPZ_EXPECT_EQ(command.value.breakpoint.id, (uint32_t)(index + 1U));
        SPZ_EXPECT_EQ(command.value.breakpoint.kind, cases[index].kind);
        SPZ_EXPECT_EQ(command.value.breakpoint.address, UINT64_C(0x1000) + (index == 1U ? 3U : index == 2U ? 4U : 0U));
        SPZ_EXPECT_EQ(command.value.breakpoint.length, cases[index].length);
        SPZ_EXPECT_EQ(command.value.breakpoint.mode, cases[index].mode);
    }
}

static void expect_rejections(void)
{
    static const char *const invalid[] = {
        " status", "status ", "status  now", "status now", "unknown",
        "status key=value", "bind", "bind pid=1", "bind mode=pid",
        "bind pid=0 mode=pid", "bind pid=-1 mode=pid", "bind pid=+1 mode=pid",
        "bind pid=4294967296 mode=pid", "bind pid=1 pid=2 mode=pid",
        "bind pid=1 mode=thread", "bind pid=1 mode=pid uid=-1",
        "bind pid=1 mode=pid uid=4294967296", "bind pid=1 mode=pid unknown=1",
        "bind pid=1 mode=pid comm=0123456789abcdef", "bind pid=1 mode=pid comm=bad/name",
        "bind pid=1 mode=pid start=18446744073709551616",
        "enable", "enable id=0", "enable id=-1", "enable id=1 id=2", "enable key=1",
        "disable", "disable id=4294967296", "clear id=1", "audit now",
        "maps now", "maps-read", "maps-read snapshot=1",
        "maps-read offset=0", "maps-read snapshot=0 offset=0",
        "maps-read snapshot=0x1 offset=0", "maps-read snapshot=1 offset=0x0",
        "maps-read snapshot=1 offset=-1",
        "maps-read snapshot=18446744073709551616 offset=0",
        "maps-read snapshot=1 offset=4294967296",
        "maps-read snapshot=1 snapshot=2 offset=0",
        "maps-read snapshot=1 offset=0 offset=1",
        "maps-read snapshot=1 offset=0 unknown=1",
        "poll after=-1", "poll after=18446744073709551616", "poll after=1 after=2", "poll key=1",
        "break", "break id=0 kind=x addr=0x1000 len=4 mode=once",
        "break id=1 kind=z addr=0x1000 len=4 mode=once",
        "break id=1 kind=x addr=1000 len=4 mode=once",
        "break id=1 kind=x addr=0x0 len=4 mode=once",
        "break id=1 kind=x addr=0xffff000000001000 len=4 mode=once",
        "break id=1 kind=x addr=0x1002 len=4 mode=once",
        "break id=1 kind=x addr=0x1000 len=2 mode=once",
        "break id=1 kind=r addr=0x1000 len=3 mode=once",
        "break id=1 kind=w addr=0x1007 len=2 mode=once",
        "break id=1 kind=rw addr=0x1000 len=8 mode=forever",
        "break id=1 kind=rw addr=0x1000 len=8 mode=once extra=1",
        "break id=1 kind=rw addr=0x1000 len=8",
    };
    size_t index;

    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        struct spz_command command;
        int result = parse_text(invalid[index], &command);

        if (result >= 0) {
            fprintf(stderr, "unexpectedly accepted command: %s\n", invalid[index]);
            spz_test_failures++;
        }
    }
}

static void expect_bounded_bytes(void)
{
    struct spz_command command;
    char overlong[SPZ_MAX_COMMAND + 1U];
    char non_ascii[] = {'s', 't', 'a', 't', 'u', (char)0x80};
    char embedded_nul[] = {'s', 't', 'a', 't', 'u', 's', '\0'};
    char embedded_newline[] = {'s', 't', 'a', 't', 'u', 's', '\n'};

    memset(overlong, 'a', sizeof(overlong));
    SPZ_EXPECT(spz_parse_command(NULL, 1U, &command) < 0);
    SPZ_EXPECT(spz_parse_command(overlong, 0U, &command) < 0);
    SPZ_EXPECT(spz_parse_command(overlong, sizeof(overlong), &command) < 0);
    SPZ_EXPECT(spz_parse_command(non_ascii, sizeof(non_ascii), &command) < 0);
    SPZ_EXPECT(spz_parse_command(embedded_nul, sizeof(embedded_nul), &command) < 0);
    SPZ_EXPECT(spz_parse_command(embedded_newline, sizeof(embedded_newline), &command) < 0);
    SPZ_EXPECT(spz_parse_command(overlong, 1U, NULL) < 0);
}

int test_command(void)
{
    expect_simple_commands();
    expect_bind_commands();
    expect_break_commands();
    expect_maps_read_commands();
    expect_rejections();
    expect_bounded_bytes();
    return 0;
}
