import os
import sys
import signal
import importlib
import click
import _fastwsgi
from pkg_resources import get_distribution

LL_DISABLED    = 0
LL_FATAL_ERROR = 1
LL_CRIT_ERROR  = 2
LL_ERROR       = 3
LL_WARNING     = 4
LL_NOTICE      = 5
LL_INFO        = 6
LL_DEBUG       = 7
LL_TRACE       = 8

NUM_WORKERS = 4
HOST = "0.0.0.0"
PORT = 5000
BACKLOG = 1024
LOGLEVEL = LL_ERROR


def run_multi_process_server(app):
    workers = []
    for _ in range(NUM_WORKERS):
        pid = os.fork()
        if pid > 0:
            workers.append(pid)
            print(f"Worker process added with PID: {pid}")
        else:
            try:
                _fastwsgi.run_server(app, HOST, PORT, BACKLOG, LOGLEVEL)
            except KeyboardInterrupt:
                exit()

    try:
        for _ in range(NUM_WORKERS):
            os.wait()
    except KeyboardInterrupt:
        print("\nStopping all workers")
        for worker in workers:
            os.kill(worker, signal.SIGINT)


def import_from_string(import_str):
    module_str, _, attrs_str = import_str.partition(":")
    if not module_str or not attrs_str:
        raise ImportError("Import string should be in the format <module>:<attribute>")

    try:
        module = importlib.import_module(module_str)
        for attr_str in attrs_str.split("."):
            module = getattr(module, attr_str)
    except AttributeError:
        raise ImportError(f'Attribute "{attrs_str}" not found in module "{module_str}"')

    return module


def print_server_details(host, port):
    print(f"\n==== FastWSGI ==== ")
    print(f"Host: {host}\nPort: {port}")
    print("==================\n")


@click.command()
@click.version_option(version=get_distribution("fastwsgi").version, message="%(version)s")
@click.option("--host", help="Host the socket is bound to.", type=str, default=HOST, show_default=True)
@click.option("-p", "--port", help="Port the socket is bound to.", type=int, default=PORT, show_default=True)
@click.option("-l", "--loglevel", help="Logging level.", type=int, default=LOGLEVEL, show_default=True)
@click.argument(
    "wsgi_app_import_string",
    type=str,
    required=True,
)
def run_from_cli(host, port, wsgi_app_import_string, loglevel):
    """
    Run FastWSGI server from CLI
    """
    try:
        wsgi_app = import_from_string(wsgi_app_import_string)
    except ImportError as e:
        print(f"Error importing WSGI app: {e}")
        sys.exit(1)

    print_server_details(host, port)
    print(f"Server listening at http://{host}:{port}")
    _fastwsgi.run_server(wsgi_app, host, port, BACKLOG, loglevel)


def run(wsgi_app, host=HOST, port=PORT, backlog=1024, loglevel=LOGLEVEL):
    print_server_details(host, port)
    print(f"Server listening at http://{host}:{port}")
    print(f"Running on PID:", os.getpid())
    _fastwsgi.run_server(wsgi_app, host, port, backlog, loglevel)
    # run_multi_process_server(wsgi_app)
