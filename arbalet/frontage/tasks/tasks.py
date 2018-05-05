from __future__ import absolute_import
from __future__ import print_function

import datetime
import sys
import time

from time import sleep

from server.extensions import celery
from celery.task.control import revoke
from celery import current_task

from scheduler_state import SchedulerState
from utils.red import redis, redis_get
from apps.flags import Flags
from apps.random_flashing import RandomFlashing
from apps.sweep_async import SweepAsync
from apps.sweep_rand import SweepRand
from apps.snake import Snake
from apps.tetris import Tetris
from apps.snap import Snap


class TestApp():
    def run(self, params):
        print('[TASK] Running a test app. Im doing nothing at all')
        while True:
            pass


def flask_log(msg):
    print(msg, file=sys.stderr)


def clear_all_task():
    celery.control.purge()
    if current_task:
        revoke(current_task.request.id, terminate=True)
    sleep(0.5)
    SchedulerState.set_current_app({})
    SchedulerState.set_event_lock(False)


@celery.task
def start_default_fap(app):
    SchedulerState.set_app_started_at()
    # app['expire_at'] = str(
    #     datetime.datetime.now())
    if 'params' not in app:
        app['params'] = {}
    app['expire_at'] = str(
        datetime.datetime.now() +
        datetime.timedelta(
            seconds=app['expires']))

    params = app['default_params'].copy()
    params.update(app['params'])

    app['task_id'] = start_default_fap.request.id
    app['is_default'] = True
    app['last_alive'] = time.time()
    app['username'] = '>>>default<<<'
    app['started_at'] = datetime.datetime.now().isoformat()

    SchedulerState.set_current_app(app)
    SchedulerState.set_event_lock(False)
    try:
        fap = globals()[app['name']](app['username'])
        fap.run(params=params)
    except Exception as e:
        print('--->APP>>')
        del fap
        print('Error when starting task ' + str(e))
        # raise e
    finally:
        del fap
        SchedulerState.set_current_app({})
        print('=====================> Close DEFAULT APP')


@celery.task
def start_fap(app):
    SchedulerState.set_app_started_at()
    app['expire_at'] = str(
        datetime.datetime.now() +
        datetime.timedelta(
            seconds=app['expires']))
    app['is_default'] = False
    app['task_id'] = start_fap.request.id
    app['started_at'] = datetime.datetime.now().isoformat()
    SchedulerState.pop_user_app_queue()
    SchedulerState.set_current_app(app)
    SchedulerState.set_event_lock(False)

    try:
        flask_log('[start_fap.apply_async] ===========> start run')
        fap = globals()[app['name']](app['username'])
        fap.run(params=app['params'], expires_at=app['expire_at'])
    except Exception as e:
        flask_log('--->APP>>')
        flask_log('Error when starting task ' + str(e))
        raise e
    finally:
        del fap
        SchedulerState.set_current_app({})
        flask_log('--======================== ENDED START_APP')


@celery.task
def start_forced_fap(fap_name=None, user_name='Anonymous', params=None):
    SchedulerState.set_app_started_at()
    app_struct = {
        'name': fap_name,
        'username': user_name,
        'params': params,
        'task_id': start_forced_fap.request.id,
        'last_alive': time.time(),
        'started_at': datetime.datetime.now().isoformat(),
        'is_default': False,
        'expire_at': str(datetime.datetime.now() + datetime.timedelta(weeks=52))}
    SchedulerState.set_current_app(app_struct)
    SchedulerState.set_event_lock(False)
    if fap_name:
        try:
            fap = globals()[fap_name](app_struct['username'])
            redis.set(SchedulerState.KEY_FORCED_APP, True)
            fap.run(params=params)
            return True
        except Exception as e:
            print('Error when starting task ' + str(e))
            raise
        finally:
            redis.set(SchedulerState.KEY_FORCED_APP, False)
            SchedulerState.set_current_app({})
    return True
