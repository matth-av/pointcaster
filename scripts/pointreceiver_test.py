#!/usr/bin/env python3
"""Load the pointreceiver library and log what it receives.

    ./pointreceiver_test.py
    ./pointreceiver_test.py tcp://192.168.1.10:9992
    ./pointreceiver_test.py --points session_1
    ./pointreceiver_test.py --messages
    ./pointreceiver_test.py tcp://192.168.1.10:9991 --messages
    ./pointreceiver_test.py --lib ../build/pointreceiver-linux-release/install/lib
"""

import argparse
import ctypes
import os

class Attribute(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p),
                ("data", ctypes.c_void_p),
                ("element_count", ctypes.c_size_t),
                ("quantisation_step", ctypes.c_float),
                ("quantisation_offset", ctypes.c_float),
                ("component_count", ctypes.c_uint32),
                ("stride", ctypes.c_uint32),
                ("element_type", ctypes.c_int)]


class PointCloudFrame(ctypes.Structure):
    _fields_ = [("point_count", ctypes.c_size_t),
                ("positions", ctypes.c_void_p),
                ("colours", ctypes.c_void_p),
                ("attributes", ctypes.POINTER(Attribute)),
                ("attribute_count", ctypes.c_size_t)]


class MessageValue(ctypes.Union):
    _fields_ = [("float_val", ctypes.c_float),
                ("int_val", ctypes.c_int),
                ("bool_val", ctypes.c_bool),
                ("bounds", ctypes.c_float * 6),
                ("_pointer", ctypes.c_void_p)]  # widest alignment in the union


class Message(ctypes.Structure):
    _fields_ = [("value_type", ctypes.c_int), ("value", MessageValue)]


parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("address", nargs="?",
                    help="pointcaster endpoint (default: tcp://127.0.0.1:9992 for "
                         "point clouds, tcp://127.0.0.1:9991 for messages)")
stream = parser.add_mutually_exclusive_group()
stream.add_argument("--points", nargs="?", const="", metavar="TOPIC",
                    help="point cloud topic to subscribe to (default: all)")
stream.add_argument("--messages", nargs="?", const="", metavar="TOPIC",
                    help="listen for messages instead of point clouds")
parser.add_argument("--lib", default=os.path.dirname(os.path.abspath(__file__)),
                    help="directory holding the library (default: beside this script)")
args = parser.parse_args()

library_name = "pointreceiver.dll" if os.name == "nt" else "libpointreceiver.so"
library_path = os.path.join(args.lib, library_name)

# load anything sitting beside it first, so the loader already has the
# dependencies when it resolves libpointreceiver's own
for name in sorted(os.listdir(args.lib)):
    if name != library_name and (".so" in name or name.endswith(".dll")):
        try:
            ctypes.CDLL(os.path.join(args.lib, name), mode=ctypes.RTLD_GLOBAL)
        except OSError:
            pass

lib = ctypes.CDLL(library_path)

context_arg = ctypes.c_void_p
lib.pointreceiver_create_context.restype = context_arg
lib.pointreceiver_destroy_context.argtypes = [context_arg]
lib.pointreceiver_status_string.argtypes = [ctypes.c_int]
lib.pointreceiver_status_string.restype = ctypes.c_char_p
lib.pointreceiver_set_client_name.argtypes = [context_arg, ctypes.c_char_p]
lib.pointreceiver_start_point_receiver.argtypes = [context_arg, ctypes.c_char_p]
lib.pointreceiver_stop_point_receiver.argtypes = [context_arg]
lib.pointreceiver_subscribe_to_point_cloud.argtypes = [context_arg, ctypes.c_char_p]
lib.pointreceiver_dequeue_point_cloud.argtypes = [
    context_arg, ctypes.c_char_p, ctypes.c_size_t,
    ctypes.POINTER(PointCloudFrame), ctypes.c_int]
lib.pointreceiver_attribute_copy_floats.argtypes = [
    ctypes.POINTER(Attribute), ctypes.POINTER(ctypes.c_float), ctypes.c_size_t]
lib.pointreceiver_start_message_receiver.argtypes = [context_arg, ctypes.c_char_p]
lib.pointreceiver_stop_message_receiver.argtypes = [context_arg]
lib.pointreceiver_subscribe_to_message.argtypes = [context_arg, ctypes.c_char_p]
lib.pointreceiver_dequeue_message.argtypes = [
    context_arg, ctypes.c_char_p, ctypes.c_size_t,
    ctypes.POINTER(Message), ctypes.c_int]

want_messages = args.messages is not None
topic = (args.messages if want_messages else args.points) or ""
# the two streamers publish on different ports, so the default follows the
# stream you asked for
endpoint = args.address or ("tcp://127.0.0.1:9991" if want_messages
                            else "tcp://127.0.0.1:9992")

context = lib.pointreceiver_create_context()
lib.pointreceiver_set_client_name(context, b"pointreceiver_test")

if want_messages:
    start, stop = lib.pointreceiver_start_message_receiver, lib.pointreceiver_stop_message_receiver
    subscribe, dequeue = lib.pointreceiver_subscribe_to_message, lib.pointreceiver_dequeue_message
    payload = Message()
else:
    start, stop = lib.pointreceiver_start_point_receiver, lib.pointreceiver_stop_point_receiver
    subscribe, dequeue = lib.pointreceiver_subscribe_to_point_cloud, lib.pointreceiver_dequeue_point_cloud
    payload = PointCloudFrame()

# from the pointreceiver_status enum in pointreceiver.h
status_ok = 0
status_timeout = 5

status = start(context, endpoint.encode())
if status != status_ok:
    raise SystemExit(f"could not start: {lib.pointreceiver_status_string(status).decode()}")

# nothing arrives until we subscribe...
# "" means all topics
subscribe(context, topic.encode())

kind = "messages" if want_messages else "point clouds"
print(f"listening for {kind} on {endpoint} [{topic or 'all topics'}], ctrl-c to stop")

value_types = ["float", "int", "string", "bool", "bounds",
               "points", "voxels", "aabbs", "contours", "unknown"]

# indexed by pointreceiver_attribute_type
attribute_type_names = ["float32", "uint8", "uint16", "uint32", "int16",
                        "int32", "int8"]


# the name pointcaster gives a per point radius, in the millimetre space the
# positions use
point_scale_attribute = "point_scale"

# sources already reported as carrying a point scale
scale_sources = set()


def read_attribute(attribute):
    # the library converts out of whatever it was stored as, so nothing here
    # needs to know about the storage type or its quantisation step
    value_count = attribute.element_count * attribute.component_count
    values = (ctypes.c_float * value_count)()
    if lib.pointreceiver_attribute_copy_floats(
            ctypes.byref(attribute), values, value_count) != status_ok:
        return None
    return values


def describe_attribute(attribute, values, preview_count=3):
    name = attribute.name.decode(errors="replace")
    if attribute.element_type >= len(attribute_type_names):
        return f"{name}: unknown element type {attribute.element_type}"

    if values is None:
        return f"{name}: values could not be read"

    value_count = len(values)
    shown = min(preview_count, value_count)
    preview = ", ".join(f"{values[i]:.4g}" for i in range(shown))
    ellipsis = ", ..." if value_count > shown else ""

    # a quantised attribute says how its raw elements map back, so the type
    # and the step can be read off together
    encoding = ""
    if attribute.quantisation_step != 1.0 or attribute.quantisation_offset != 0.0:
        bytes_per_point = attribute.stride
        encoding = (f" step {attribute.quantisation_step:.6g}"
                    f" offset {attribute.quantisation_offset:.6g}"
                    f" ({bytes_per_point} B/point)")

    return (f"{name}: {attribute.element_count} x "
            f"{attribute_type_names[attribute.element_type]}{encoding} "
            f"[{preview}{ellipsis}]")


def report_point_scale(source, values):
    """Log the range of scales, and the source the first time it sends one."""
    if values is None or len(values) == 0:
        return
    if source not in scale_sources:
        scale_sources.add(source)
        print(f"  point scale on '{source}' "
              f"(subscribed to {topic or 'all topics'} on {endpoint})")
    print(f"  point scale {min(values):.4g} - {max(values):.4g} mm")

address = ctypes.create_string_buffer(256)
try:
    while True:
        status = dequeue(context, address, len(address), ctypes.byref(payload), 100)
        if status == status_timeout:
            continue
        if status != status_ok:
            print("error:", lib.pointreceiver_status_string(status).decode())
            continue
        source = address.value.decode(errors="replace")


        if want_messages:
            name = value_types[payload.value_type] if payload.value_type < len(value_types) else "?"
            print(f"{source}: {name}")
        else:
            print(f"{payload.point_count} points from '{source}'")
            for i in range(payload.attribute_count):
                attribute = payload.attributes[i]
                values = read_attribute(attribute)
                print(f"    {describe_attribute(attribute, values)}")
                if attribute.name.decode(
                        errors="replace") == point_scale_attribute:
                    report_point_scale(source, values)
except KeyboardInterrupt:
    print()

stop(context)
lib.pointreceiver_destroy_context(context)
