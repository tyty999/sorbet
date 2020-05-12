def snapshot_tests(ruby = None, tests = []):
    """
    Define a bunch of snapshot tests all at once. The paths must all be of the
    form "partial/<test>" or "total/<test>".
    """

    if ruby == None:
        fail(msg = "No ruby version specified in snapshot_tests")

    name = "snapshot-{}".format(ruby)
    update_name = "update-{}".format(ruby)

    test_rules = []
    update_rules = []

    for test_path in tests:
        res = _snapshot_test(test_path, ruby)
        test_rules.append(res["test_name"])

        if res.get("update_name") != None:
            update_rules.append(res["update_name"])

    native.test_suite(
        name = name,
        tests = test_rules,
        # NOTE: ensure that this rule isn't caught in //...
        tags = ["manual"],
    )

    native.test_suite(
        name = update_name,
        tests = update_rules,
        # NOTE: ensure that this rule isn't caught in //...
        tags = ["manual"],
    )

def _build_snapshot_artifacts(ctx):
    ctx.actions.run_shell(
        outputs = [ctx.outputs.archive],
        inputs = ctx.files.srcs,
        tools = ctx.files._run_one,
        command = """
        log_file="$(mktemp)"
        cleanup() {{
            rm -f "$log_file"
        }}

        trap cleanup EXIT

        if ! {run_one} {ruby} {archive} {test_name} &> "$log_file" ; then
            cat "$log_file"
            exit 1
        fi
        """.format(
            run_one = ctx.executable._run_one.path,
            ruby = ctx.attr.ruby,
            archive = ctx.outputs.archive.path,
            test_name = ctx.attr.test_name,
        ),
        progress_message = "Running {} ({})".format(ctx.attr.test_name, ctx.attr.ruby),
    )

    runfiles = ctx.runfiles(files = ctx.files.srcs + ctx.files._run_one)

    return [DefaultInfo(
        runfiles = runfiles,
    )]

build_snapshot_artifacts = rule(
    implementation = _build_snapshot_artifacts,
    attrs = {
        "_run_one": attr.label(
            executable = True,
            default = ":run_one",
            cfg = "target",
        ),
        "ruby": attr.string(),
        "test_name": attr.string(),
        "srcs": attr.label_list(
            allow_empty = False,
            allow_files = True,
        ),
        "archive": attr.output(),
    },
)

def _snapshot_test(test_path, ruby):
    """
    test_path is of the form `total/test` or `partial/test`.
    ruby is the version of ruby to use (named in //third_party/externals.bzl)
    """

    res = {}
    actual = "{}/actual_{}.tar".format(test_path, ruby)

    build_snapshot_artifacts(
        name = "actual_{}/{}".format(ruby, test_path),
        ruby = "ruby_2_6",
        test_name = test_path,
        srcs = native.glob([
            "{}/src/**/*".format(test_path),
            "{}/expected/**/*".format(test_path),
            "{}/gems/**/*".format(test_path),
        ]),
        archive = actual,
    )

    test_name = "test_{}/{}".format(ruby, test_path)

    native.sh_test(
        name = test_name,
        srcs = ["check_one.sh"],
        data = [actual] + native.glob([
            "{}/src/**/*".format(test_path),
            "{}/expected/**/*".format(test_path),
            "{}/gems/**/*".format(test_path),
        ]),
        deps = [":logging", ":validate_utils"],
        args = [
            "$(location {})".format(actual),
            test_path,
        ],

        # NOTE: this is manual to avoid being caught with `//...`
        tags = ["manual"],
    )

    res["test_name"] = test_name

    # Generate an update rule if the test has an expected directory
    expected = native.glob(
        ["{}/expected".format(test_path)],
        exclude_directories = 0,
    )

    if len(expected) > 0:
        update_name = "update_{}/{}".format(ruby, test_path)

        native.sh_test(
            name = update_name,
            data = [actual] + expected,
            deps = [":logging", ":validate_utils"],
            srcs = ["update_one.sh"],
            args = [
                "$(location {})".format(actual),
                test_path,
            ],

            # Don't run this rule remotely, or in the sandbox
            local = True,

            # NOTE: these tags cause this test to be skipped by `bazel test //...`,
            # and not run in the sandbox:
            #
            # "manual"   - don't include this rule in `bazel test //...`
            # "external" - unconditionally execute this rule
            tags = [
                "manual",
                "external",
            ],
        )

        res["update_name"] = update_name

    return res
