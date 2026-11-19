#
# Build rules for the Ebb watchapp + background worker.
#
# ER_DEMO=1 pebble build   -> compiles the synthetic-data adapter so the app can be
#                             exercised in the emulator, which has no health data.
# ER_DEBUG=1 pebble build  -> on-wrist debugging: logs what the score was built from and
#                             starts an HRV measurement a few seconds after every launch.
# ER_WLOG=1 pebble build   -> the worker logs its bursts and scoring runs. ER_PPI=1 adds every
#                             raw beat-to-beat interval. Both cost worker memory that
#                             scoring needs, so use them to watch the sampler only.
#
import os
import os.path

top = '.'
out = 'build'


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    ctx.load('pebble_sdk')


def build(ctx):
    ctx.load('pebble_sdk')

    demo = os.environ.get('ER_DEMO') == '1'
    build_worker = os.path.exists('worker_src')
    binaries = []

    cached_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        if demo:
            ctx.env.append_value('DEFINES', 'ER_DEMO=1')
        if os.environ.get('ER_DEBUG') == '1':
            ctx.env.append_value('DEFINES', 'ER_DEBUG=1')
        if os.environ.get('ER_PPI') == '1':
            ctx.env.append_value('DEFINES', 'ER_PPI=1')
        if os.environ.get('ER_WLOG') == '1':
            ctx.env.append_value('DEFINES', 'ER_WLOG=1')
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf, bin_type='app')

        if build_worker:
            worker_elf = '{}/pebble-worker.elf'.format(ctx.env.BUILD_DIR)
            binaries.append({'platform': platform, 'app_elf': app_elf, 'worker_elf': worker_elf})
            ctx.pbl_build(source=ctx.path.ant_glob('worker_src/c/**/*.c'),
                          target=worker_elf,
                          bin_type='worker')
        else:
            binaries.append({'platform': platform, 'app_elf': app_elf})
    ctx.env = cached_env

    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries,
                   js=ctx.path.ant_glob(['src/pkjs/**/*.js',
                                         'src/pkjs/**/*.json',
                                         'src/common/**/*.js']),
                   js_entry_file='src/pkjs/index.js')
