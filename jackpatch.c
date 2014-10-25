#include <Python.h>
#include "structmember.h"
#include <stdarg.h>
#include <pthread.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/transport.h>

// the size of buffers to use for various purposes
#define BUFFER_SIZE 1024
// the maximum number of MIDI I/O ports to allow for a client
#define MAX_PORTS_PER_CLIENT 256
// define whether to emit warnings when in a JACK processing callback
//  (normally not a great idea because it can produce floods of warnings, but 
//   useful when debugging)
#define WARN_IN_PROCESS 0

// ERROR HANDLING *************************************************************

static PyObject *JackError;
static void _error(const char *format, ...) {
  static char message[BUFFER_SIZE];
  va_list ap;
  va_start(ap, format);
  vsnprintf(message, BUFFER_SIZE, format, ap);
  va_end(ap);
  PyErr_SetString(JackError, message);
}
static void _warn(const char *format, ...) {
  static char message[BUFFER_SIZE];
  va_list ap;
  va_start(ap, format);
  vsnprintf(message, BUFFER_SIZE, format, ap);
  va_end(ap);
  PyErr_WarnEx(PyExc_RuntimeWarning, message, 2);
}

// STORAGE TYPES **************************************************************

// define a struct to store queued MIDI messages, with a self-pointer that can
//  be used to manage the queue as a linked list
typedef struct {
  void *next;
  jack_port_t *port;
  jack_nframes_t time;
  size_t data_size;
  // the data goes at the end so we can allocate a variable number of bytes 
  //  for it depending on the message length; if you want to add more members
  //  to the struct, do it somewhere above here
  unsigned char data[];
} Message;

static PyTypeObject PortType;
typedef struct {
  PyObject_HEAD
  // public attributes
  PyObject *name;
  PyObject *client;
  PyObject *flags;
  // private stuff
  jack_port_t *_port;
  int _is_mine;
} Port;

static PyTypeObject TransportType;
typedef struct {
  PyObject_HEAD
  // public attributes
  PyObject *client;
  // private stuff
} Transport;

static PyTypeObject ClientType;
typedef struct {
  PyObject_HEAD
  // public attributes
  PyObject *name;
  PyObject *is_open;
  PyObject *is_active;
  PyObject *transport;
  // private stuff
  jack_client_t *_client;
  int _send_port_count;
  jack_port_t **_send_ports;
  Message *_midi_send_queue_head;
  pthread_mutex_t _midi_send_queue_lock;
  int _receive_port_count;
  jack_port_t **_receive_ports;
  Message *_midi_receive_queue_head;
  Message *_midi_receive_queue_tail;
  pthread_mutex_t _midi_receive_queue_lock;
} Client;

// FORWARD DECLARATIONS *******************************************************

static PyObject * Port_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static int Port_init(Port *self, PyObject *args, PyObject *kwds);
static PyObject * Transport_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static int Transport_init(Transport *self, PyObject *args, PyObject *kwds);

// CLIENT *********************************************************************

static PyObject *
Client_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  Client *self;
  self = (Client *)type->tp_alloc(type, 0);
  if (self != NULL) {
    // attributes
    self->name = Py_None;
    self->is_open = Py_False;
    self->is_active = Py_False;
    self->transport = Py_None;
    // private stuff
    self->_send_port_count = 0;
    self->_send_ports = malloc(sizeof(jack_port_t *) * MAX_PORTS_PER_CLIENT);
    self->_midi_send_queue_head = NULL;
    self->_receive_port_count = 0;
    self->_receive_ports = malloc(sizeof(jack_port_t *) * MAX_PORTS_PER_CLIENT);
    self->_midi_receive_queue_head = NULL;
    self->_midi_receive_queue_tail = NULL;
  }
  return((PyObject *)self);
}

static int
Client_init(Client *self, PyObject *args, PyObject *kwds) {
  PyObject *name=NULL, *tmp;
  static char *kwlist[] = {"name", NULL};
  if (! PyArg_ParseTupleAndKeywords(args, kwds, "S", kwlist, 
                                    &name))
    return(-1);
  tmp = self->name;
  Py_INCREF(name);
  self->name = name;
  Py_XDECREF(tmp);
  // add a transport for the client
  Transport *transport = (Transport *)Transport_new(&TransportType, NULL, NULL);
  if (transport != NULL) {
    Transport_init(transport, Py_BuildValue("(O)", self), Py_BuildValue("{}"));
    tmp = self->transport;
    Py_INCREF(transport);
    self->transport = (PyObject *)transport;
    Py_XDECREF(tmp);
  }
  return(0);
}

// make sure the client is connected to the JACK server
static PyObject *
Client_open(Client *self) {
  if (self->_client == NULL) {
    jack_status_t status;
    self->_client = jack_client_open(PyString_AsString(self->name), 
      JackNoStartServer, &status);
    if ((status & JackServerFailed) != 0) {
      _error("%s", "Failed to connect to the JACK server");
      return(NULL);
    }
    else if ((status & JackServerError) != 0) {
      _error("%s", "Failed to communicate with the JACK server");
      return(NULL);
    }
    else if ((status & JackFailure) != 0) {
      _error("%s", "Failed to create a JACK client");
      return(NULL);
    }
    self->is_open = Py_True;
  }
  Py_RETURN_NONE;
}

// close a client's connection to JACK server
static PyObject *
Client_close(Client *self) {
  if (self->_client != NULL) {
    jack_client_close(self->_client);
    self->_client = NULL;
    self->is_open = Py_False;
  }
  Py_RETURN_NONE;
}

// send queued messages for one of a client's ports
static void
Client_send_messages_for_port(Client *self, jack_port_t *port, 
                              jack_nframes_t nframes) {
  unsigned char *buffer;
  // get a writable buffer for the port
  void *port_buffer = jack_port_get_buffer(port, nframes);
  if (port_buffer == NULL) {
    #ifdef WARN_IN_PROCESS
      _warn("Failed to get port buffer for sending");
    #endif
    return;
  }
  // clear the buffer for writing
  jack_midi_clear_buffer(port_buffer);
  // if the queue is empty, we can skip traversing it
  if (self->_midi_send_queue_head == NULL) return;
  // ensure the send queue isn't changed while we're traversing it
  pthread_mutex_t *lock = &(self->_midi_send_queue_lock);
  pthread_mutex_lock(lock);
  // send messages
  Message *last = NULL;
  Message *message = self->_midi_send_queue_head;
  Message *next = NULL;
  int port_send_count = 0;
  jack_nframes_t last_time = 0;
  while (message != NULL) {
    // get messages for the current port
    if (message->port == port) {
      // if this message overlaps with another's time, 
      //  delay it enough that they don't overlap
      if ((port_send_count > 0) && (message->time <= last_time)) {
        message->time = last_time + 1;
      }
      // get messages that fall in the current block
      if (message->time < nframes) {
        buffer = jack_midi_event_reserve(
          port_buffer, message->time, message->data_size);
        if (buffer != NULL) {
          memcpy(buffer, message->data, message->data_size);
        }
        else {
          #ifdef WARN_IN_PROCESS
            _warn("Failed to allocate a buffer to write a message into");
          #endif
        }
        // keep track of the time of the last message
        port_send_count++;
        last_time = message->time;
        // remove the message from the queue once sent
        next = (Message *)message->next;
        message->next = NULL;
        if (last != NULL) last->next = next;
        if (message == self->_midi_send_queue_head) {
          self->_midi_send_queue_head = next;
        }
        free(message);
        message = next;
        continue;
      }
      // shift the times of remaining messages so they get sent in later blocks
      message->time -= nframes;
    }
    // traverse the linked list
    last = message;
    message = (Message *)message->next;
  }
  // release the queue for changes
  pthread_mutex_unlock(lock);
}

// receive and enqueue messages for one of a client's ports
static void
Client_receive_messages_for_port(Client *self, jack_port_t *port, 
                                 jack_nframes_t nframes) {
  int result;
  Message *message = NULL;
  // get a readable buffer for the port
  void *port_buffer = jack_port_get_buffer(port, nframes);
  if (port_buffer == NULL) {
    #ifdef WARN_IN_PROCESS
      _warn("Failed to get port buffer for receiving");
    #endif
    return;
  }
  // get the number of events to receive for this block
  int event_count = jack_midi_get_event_count(port_buffer);
  // if there are no events for the port, we can skip receiving
  if (event_count == 0) return;
  // get the time at the start of the block so event times are synced 
  //  to the transport
  jack_position_t pos;
  jack_transport_query(self->_client, &pos);
  jack_nframes_t start_frame = pos.frame;
  // make sure the queue doesn't get changed while we're adding to it
  pthread_mutex_t *lock = &(self->_midi_receive_queue_lock);
  pthread_mutex_lock(lock);
  // receive events
  jack_midi_event_t event;
  for (int i = 0; i < event_count; i++) {
    result = jack_midi_event_get(&event, port_buffer, i);
    if (result != 0) {
      #ifdef WARN_IN_PROCESS
        _warn("Failed to get an event at index %d", i);
      #endif
      continue;
    }
    // allocate a message to store the event
    message = (Message *)malloc(sizeof(Message) + 
                (sizeof(unsigned char) * event.size));
    if (message == NULL) {
      #ifdef WARN_IN_PROCESS
        _warn("Failed to allocate memory for the message at index %d "
              "with data size %d", i, event.size);
      #endif
      continue;
    }
    // set up the message
    message->next = NULL;
    message->port = port;
    message->time = start_frame + event.time;
    message->data_size = event.size;
    memcpy(message->data, event.buffer, event.size);
    // attach it to the queue
    if (self->_midi_receive_queue_head == NULL) {
      self->_midi_receive_queue_head = message;
    }
    if (self->_midi_receive_queue_tail != NULL) {
      self->_midi_receive_queue_tail->next = message;
    }
    self->_midi_receive_queue_tail = message;
  }
  // free the queue for updates
  pthread_mutex_unlock(lock);
}

// process a block of events for a client
static int
Client_process(jack_nframes_t nframes, void *self_ptr) {
  int i;
  jack_port_t *port;
  Client *self = (Client *)self_ptr;
  if (self == NULL) return(-1);
  // send queued messages
  for (i = 0; i < self->_send_port_count; i++) {
    port = self->_send_ports[i];
    Client_send_messages_for_port(self, port, nframes);
  }
  // enqueue received messages
  for (i = 0; i < self->_receive_port_count; i++) {
    port = self->_receive_ports[i];
    Client_receive_messages_for_port(self, port, nframes);
  }
  return(0);
}

// start processing events for a client
static PyObject *
Client_activate(Client *self) {
  int result;
  Client_open(self);
  if (self->_client == NULL) return(NULL);
  if (self->is_active != Py_True) {
    // connect a callback for processing MIDI messages
    result = jack_set_process_callback(self->_client, Client_process, self);
    if (result != 0) {
      _warn("Failed to set a callback for the JACK client (error %i), "
            "MIDI send/receive will be disabled", result);
    }
    result = jack_activate(self->_client);
    if (result != 0) {
      _error("Failed to activate the JACK client (error %i)", result);
      return(NULL);
    }
    self->is_active = Py_True;
  }
  Py_RETURN_NONE;
}

// stop processing events for a client
static PyObject *
Client_deactivate(Client *self) {
  if ((self->is_active == Py_True) && (self->_client != NULL)) {
    int result = jack_deactivate(self->_client);
    if (result != 0) {
      _error("Failed to deactivate the JACK client (error %i)", result);
      return(NULL);
    }
    self->is_active = Py_False;
  }
  Py_RETURN_NONE;
}

// use a client to list ports (this will also list ports owned by other 
//  clients unless the "mine" parameter is set to True)
static PyObject *
Client_get_ports(Client *self, PyObject *args, PyObject *kwds) {
  const char *name_pattern = NULL;
  const char *type_pattern = NULL;
  unsigned long flags = 0;
  PyObject *mine = Py_False;
  static char *kwlist[] = {"name_pattern", "type_pattern", "flags", "mine", NULL};
  if (! PyArg_ParseTupleAndKeywords(args, kwds, "|sskO", kwlist, 
                                    &name_pattern, &type_pattern, &flags, &mine))
    return(NULL);
  // see if we're only listing the ports of this client
  int mine_only = PyObject_IsTrue(mine);
  // make sure we're connected to JACK
  Client_open(self);
  if (self->_client == NULL) return(NULL);
  // get a list of port names
  const char **port_name = jack_get_ports(self->_client, 
    name_pattern, type_pattern, flags);
  // convert the port names into a list of Port objects
  PyObject *return_list;
  return_list = PyList_New(0);
  if (port_name != NULL) {
    while (*port_name != NULL) {
      // discard outside ports if requested
      if (mine_only) {
        jack_port_t *port_handle = jack_port_by_name(self->_client, *port_name);
        if ((port_handle == NULL) || 
            (! jack_port_is_mine(self->_client, port_handle))) {
          free((void *)port_name++);
          continue;
        }
      }
      Port *port = (Port *)Port_new(&PortType, NULL, NULL);
      if (port != NULL) {
        PyObject *name = PyString_FromString(*port_name);
        Port_init(port, Py_BuildValue("(O,S)", self, name), Py_BuildValue("{}"));
        Py_XDECREF(name);
        if (PyList_Append(return_list, (PyObject *)port) < 0) {
          _error("Failed to append a port to the list");
          Py_DECREF(return_list);
          return(NULL);
        }
      }
      free((void *)port_name++);
    }
  }
  return(return_list);
}

// use a client to make a connection between two ports
static PyObject *
Client_connect(Client *self, PyObject *args, PyObject *kwds) {
  Port *source = NULL;
  Port *destination = NULL;
  static char *kwlist[] = {"source", "destination", NULL};
  if (! PyArg_ParseTupleAndKeywords(args, kwds, "O!O!", kwlist, 
                                  &PortType, &source, &PortType, &destination))
    return(NULL);
  Client_activate(self);
  if (self->is_active != Py_True) return(NULL);
  int result = jack_connect(self->_client, 
    PyString_AsString(source->name), 
    PyString_AsString(destination->name));
  if ((result == 0) || (result == EEXIST)) Py_RETURN_TRUE;
  else {
    _warn("Failed to connect JACK ports (error %i)", result);
    Py_RETURN_FALSE;
  }
}

// use a client to break the connection between two ports, if any
static PyObject *
Client_disconnect(Client *self, PyObject *args, PyObject *kwds) {
  Port *source = NULL;
  Port *destination = NULL;
  static char *kwlist[] = {"source", "destination", NULL};
  if (! PyArg_ParseTupleAndKeywords(args, kwds, "O!O!", kwlist, 
                                  &PortType, &source, &PortType, &destination))
    return(NULL);
  Client_activate(self);
  if (self->is_active != Py_True) return(NULL);
  int result = jack_disconnect(self->_client, 
    PyString_AsString(source->name), 
    PyString_AsString(destination->name));
  if ((result == 0) || (result == EEXIST)) Py_RETURN_TRUE;
  else {
    _warn("Failed to disconnect JACK ports (error %i)", result);
    Py_RETURN_FALSE;
  }
}

// clean up allocated data for a client
static void
Client_dealloc(Client* self) {
  Client_close(self);
  // invalidate references to ports by zeroing the counts
  self->_send_port_count = 0;
  self->_receive_port_count = 0;
  // remove all events from the send and receive queues
  Message *message = NULL;
  Message *next = NULL;
  message = self->_midi_receive_queue_head;
  self->_midi_receive_queue_head = NULL;
  self->_midi_receive_queue_tail = NULL;
  while (message != NULL) {
    next = message->next;
    free(message);
    message = next;
  }
  message = self->_midi_send_queue_head;
  self->_midi_send_queue_head = NULL;
  while (message != NULL) {
    next = message->next;
    free(message);
    message = next;
  }
  Py_XDECREF(self->name);  
  self->ob_type->tp_free((PyObject*)self);
}

static PyMemberDef Client_members[] = {
  {"name", T_OBJECT_EX, offsetof(Client, name), READONLY,
   "The client's unique name"},
  {"is_open", T_OBJECT_EX, offsetof(Client, is_open), READONLY,
   "Whether the client is connected to JACK"},
  {"is_active", T_OBJECT_EX, offsetof(Client, is_active), READONLY,
   "Whether the client is activated to send and receive data"},
  {"transport", T_OBJECT_EX, offsetof(Client, transport), READONLY,
   "A Transport that interfaces with the JACK transport via this client"},
  {NULL}  /* Sentinel */
};

static PyMethodDef Client_methods[] = {
    {"open", (PyCFunction)Client_open, METH_NOARGS,
      "Ensure the client is connected to JACK"},
    {"close", (PyCFunction)Client_close, METH_NOARGS,
      "Ensure the client is not connected to JACK"},
    {"activate", (PyCFunction)Client_activate, METH_NOARGS,
      "Ensure the client is ready to send and receive data"},
    {"deactivate", (PyCFunction)Client_deactivate, METH_NOARGS,
      "Ensure the client cannot send and receive data"},
    {"get_ports", (PyCFunction)Client_get_ports, METH_VARARGS | METH_KEYWORDS,
      "Get a list of available ports"},
    {"connect", (PyCFunction)Client_connect, METH_VARARGS | METH_KEYWORDS,
      "Connect a source and destination port"},
    {"disconnect", (PyCFunction)Client_disconnect, METH_VARARGS | METH_KEYWORDS,
      "Disconnect a source and destination port"},
    {NULL, NULL, 0, NULL}  /* Sentinel */
};

static PyTypeObject ClientType = {
    PyObject_HEAD_INIT(NULL)
    0,                             /*ob_size*/
    "jackpatch.Client",            /*tp_name*/  
    sizeof(Client),                /*tp_basicsize*/
    0,                             /*tp_itemsize*/
    (destructor)Client_dealloc,    /*tp_dealloc*/
    0,                             /*tp_print*/
    0,                             /*tp_getattr*/
    0,                             /*tp_setattr*/
    0,                             /*tp_compare*/
    0,                             /*tp_repr*/
    0,                             /*tp_as_number*/
    0,                             /*tp_as_sequence*/
    0,                             /*tp_as_mapping*/
    0,                             /*tp_hash */
    0,                             /*tp_call*/
    0,                             /*tp_str*/
    0,                             /*tp_getattro*/
    0,                             /*tp_setattro*/
    0,                             /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Represents a JACK client",    /* tp_doc */
    0,		                         /* tp_traverse */
    0,		                         /* tp_clear */
    0,		                         /* tp_richcompare */
    0,		                         /* tp_weaklistoffset */
    0,		                         /* tp_iter */
    0,		                         /* tp_iternext */
    Client_methods,                /* tp_methods */
    Client_members,                /* tp_members */
    0,                             /* tp_getset */
    0,                             /* tp_base */
    0,                             /* tp_dict */
    0,                             /* tp_descr_get */
    0,                             /* tp_descr_set */
    0,                             /* tp_dictoffset */
    (initproc)Client_init,         /* tp_init */
    0,                             /* tp_alloc */
    Client_new,                    /* tp_new */
};

// TRANSPORT ******************************************************************

static PyObject *
Transport_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  Transport *self;
  self = (Transport *)type->tp_alloc(type, 0);
  if (self != NULL) {
    self->client = Py_None;
  }
  return((PyObject *)self);
}

static int
Transport_init(Transport *self, PyObject *args, PyObject *kwds) {
  PyObject *client=NULL, *tmp;
  static char *kwlist[] = {"client", NULL};
  if (! PyArg_ParseTupleAndKeywords(args, kwds, "O!", kwlist, 
                                    &ClientType, &client))
    return(-1);
  tmp = self->client;
  Py_INCREF(client);
  self->client = client;
  Py_XDECREF(tmp);
  return(0);
}

// clean up allocated data for a transport
static void
Transport_dealloc(Transport* self) {
  Py_XDECREF(self->client);  
  self->ob_type->tp_free((PyObject*)self);
}

// get the current time of the transport
static PyObject *
Transport_get_time(Transport *self, void *closure) {
  // make sure the client is connected to JACK
  Client *client = (Client *)self->client;
  Client_open(client);
  if (client->_client == NULL) return(NULL);
  // get the current transport position in frames
  jack_nframes_t nframes = jack_get_current_transport_frame(client->_client);
  // convert it to seconds and return
  jack_nframes_t sample_rate = jack_get_sample_rate(client->_client);
  double time = (double)nframes / (double)sample_rate;
  return(PyFloat_FromDouble(time));
}
// set the current time of the transport
static int
Transport_set_time(Transport *self, PyObject *value, void *closure) {
  if (value == NULL) {
    PyErr_SetString(PyExc_TypeError, "Cannot delete the time attribute");
    return -1;
  }
  double time = PyFloat_AsDouble(value);
  if (PyErr_Occurred()) return(-1);
  // guard against negative transport locations
  if (time < 0.0) time = 0.0;
  // make sure the client is connected to JACK
  Client *client = (Client *)self->client;
  Client_open(client);
  if (client->_client == NULL) return(-1);
  // convert the time to frames
  jack_nframes_t sample_rate = jack_get_sample_rate(client->_client);
  jack_nframes_t nframes = (jack_nframes_t)((double)sample_rate * time);
  // request that JACK update the position
  int result = jack_transport_locate(client->_client, nframes);
  // warn if that failed
  if (result != 0) {
    _warn("Failed to set transport location to %f (error %d)", time, result);
  }
  return(0);
}

// start the transport rolling
static PyObject *
Transport_start(Transport *self) {
  // make sure the client is connected to JACK
  Client *client = (Client *)self->client;
  Client_open(client);
  if (client->_client == NULL) return(NULL);
  // set the transport state
  jack_transport_start(client->_client);
  Py_RETURN_NONE;
}
// stop the transport rolling
static PyObject *
Transport_stop(Transport *self) {
  // make sure the client is connected to JACK
  Client *client = (Client *)self->client;
  Client_open(client);
  if (client->_client == NULL) return(NULL);
  // set the transport state
  jack_transport_stop(client->_client);
  Py_RETURN_NONE;
}

// get whether the transport is rolling
static PyObject *
Transport_get_is_rolling(Transport *self, void *closure) {
  // make sure the client is connected to JACK
  Client *client = (Client *)self->client;
  Client_open(client);
  if (client->_client == NULL) return(NULL);
  // get the transport state
  jack_transport_state_t state = jack_transport_query(client->_client, NULL);
  if (state == JackTransportRolling) Py_RETURN_TRUE;
  else Py_RETURN_FALSE;
}
// start/stop the transport by setting its rolling state
static int
Transport_set_is_rolling(Transport *self, PyObject *value, void *closure) {
  if (PyObject_IsTrue(value)) {
    Transport_start(self);
  }
  else {
    Transport_stop(self);
  }
  return(0);
}

static PyMemberDef Transport_members[] = {
  {"client", T_OBJECT_EX, offsetof(Transport, client), READONLY,
   "The client the transport uses to communicate with JACK"},
  {NULL}  /* Sentinel */
};

static PyGetSetDef Transport_getset[] = {
  {"time", (getter)Transport_get_time, (setter)Transport_set_time, 
    "The transport's current position in seconds", NULL},
  {"is_rolling", (getter)Transport_get_is_rolling, 
                 (setter)Transport_set_is_rolling, 
    "Whether the transport is currently advancing its time", NULL},
  {NULL}  /* Sentinel */
};

static PyMethodDef Transport_methods[] = {

    {"start", (PyCFunction)Transport_start, METH_NOARGS,
      "Start the transport rolling if it isn't already"},
    {"stop", (PyCFunction)Transport_stop, METH_NOARGS,
      "Stop the transport rolling if it is"},
    {NULL, NULL, 0, NULL}  /* Sentinel */
};

static PyTypeObject TransportType = {
    PyObject_HEAD_INIT(NULL)
    0,                             /*ob_size*/
    "jackpatch.Transport",         /*tp_name*/  
    sizeof(Transport),             /*tp_basicsize*/
    0,                             /*tp_itemsize*/
    (destructor)Transport_dealloc, /*tp_dealloc*/
    0,                             /*tp_print*/
    0,                             /*tp_getattr*/
    0,                             /*tp_setattr*/
    0,                             /*tp_compare*/
    0,                             /*tp_repr*/
    0,                             /*tp_as_number*/
    0,                             /*tp_as_sequence*/
    0,                             /*tp_as_mapping*/
    0,                             /*tp_hash */
    0,                             /*tp_call*/
    0,                             /*tp_str*/
    0,                             /*tp_getattro*/
    0,                             /*tp_setattro*/
    0,                             /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Represents a JACK transport", /* tp_doc */
    0,                             /* tp_traverse */
    0,                             /* tp_clear */
    0,                             /* tp_richcompare */
    0,                             /* tp_weaklistoffset */
    0,                             /* tp_iter */
    0,                             /* tp_iternext */
    Transport_methods,             /* tp_methods */
    Transport_members,             /* tp_members */
    Transport_getset,              /* tp_getset */
    0,                             /* tp_base */
    0,                             /* tp_dict */
    0,                             /* tp_descr_get */
    0,                             /* tp_descr_set */
    0,                             /* tp_dictoffset */
    (initproc)Transport_init,      /* tp_init */
    0,                             /* tp_alloc */
    Transport_new,                 /* tp_new */
};

// PORT ***********************************************************************

static void
Port_dealloc(Port* self) {
  Py_XDECREF(self->name);
  Py_XDECREF(self->client);
  self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
Port_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  Port *self;
  self = (Port *)type->tp_alloc(type, 0);
  if (self != NULL) {
    self->name = Py_None;
  }
  return((PyObject *)self);
}

static int
Port_init(Port *self, PyObject *args, PyObject *kwds) {
  char *requested_name = NULL;
  PyObject *name = NULL, *tmp;
  Client *client = NULL;
  unsigned long flags = 0;
  static char *kwlist[] = { "client", "name", "flags", NULL };
  if (! PyArg_ParseTupleAndKeywords(args, kwds, "O!s|k", kwlist, 
                                    &ClientType, &client, &requested_name, &flags))
    return(-1);
  // hold a reference to the underlying client so it never goes away while its
  //  ports are being used
  PyObject *client_obj = (PyObject *)client;
  tmp = self->client;
  Py_INCREF(client_obj);
  self->client = client_obj;
  Py_XDECREF(tmp);
  // make sure the client is connected
  Client_open(client);
  if (client->_client == NULL) return(-1);
  // see if a port already exists with this name
  self->_is_mine = 0;
  self->_port = jack_port_by_name(client->_client, requested_name);
  // if there's no such port, we need to create one
  if (self->_port == NULL) {
    self->_is_mine = 1;
    self->_port = jack_port_register(
      client->_client, requested_name, 
        JACK_DEFAULT_MIDI_TYPE, flags, 0);
    // store the port with the client so it can manage MIDI for it
    if ((flags & JackPortIsInput) != 0) {
      if (client->_receive_port_count >= MAX_PORTS_PER_CLIENT) {
        _warn("Failed to manage the port named \"%s\" because client has "
              "too many ports. MIDI will be disabled for that port.", 
              jack_port_name(self->_port));
      }
      else {
        client->_receive_ports[client->_receive_port_count] = self->_port;
        client->_receive_port_count++;
      }
    }
    else if ((flags & JackPortIsOutput) != 0) {
      if (client->_send_port_count >= MAX_PORTS_PER_CLIENT) {
        _warn("Failed to manage the port named \"%s\" because client has "
              "too many ports. MIDI will be disabled for that port.", 
              jack_port_name(self->_port));
      }
      else {
        client->_send_ports[client->_send_port_count] = self->_port;
        client->_send_port_count++;
      }
    }
  }
  if (self->_port == NULL) {
    _error("Failed to create a JACK port named \"%s\"", requested_name);
    return(-1);
  }
  // store the actual name of the port
  tmp = self->name;
  self->name = PyString_FromString(jack_port_name(self->_port));
  Py_XDECREF(tmp);
  // store the actual flags of the port
  tmp = self->flags;
  self->flags = Py_BuildValue("i", jack_port_flags(self->_port));
  Py_XDECREF(tmp);
  return(0);
}

static PyObject *
Port_send(Port *self, PyObject *args) {
  int status;
  PyObject *data;
  double time = 0.0;
  if (! PyArg_ParseTuple(args, "O|d", &data, &time)) return(NULL);
  if (! PySequence_Check(data)) {
    PyErr_SetString(PyExc_TypeError, 
      "Port.send expects argument 1 to be a sequence");
    return(NULL);
  }
  if (! self->_is_mine) {
    _error("Only ports created by jackpatch can send MIDI messages");
    return(NULL);
  }
  int flags = jack_port_flags(self->_port);
  if ((flags & JackPortIsOutput) == 0) {
    _error("Only output ports can send MIDI messages");
    return(NULL);
  }
  // the client needs to be activated for sending to work
  Client *client = (Client *)self->client;
  Client_activate(client);
  // get the current sample rate for time conversions
  jack_nframes_t sample_rate = jack_get_sample_rate(client->_client);
  // store the message
  size_t bytes = PySequence_Size(data);
  Message *message = malloc(sizeof(Message) + (sizeof(unsigned char) * bytes));
  if (message == NULL) {
    _error("Failed to allocate memory for MIDI data");
    return(NULL);
  }
  message->next = NULL;
  message->port = self->_port;
  message->time = (jack_nframes_t)(time * (double)sample_rate);
  message->data_size = bytes;
  unsigned char *mdata = message->data;
  long value;
  for (size_t i = 0; i < bytes; i++) {
    value = PyInt_AsLong(PySequence_ITEM(data, i));
    if ((value == -1) && (PyErr_Occurred())) return(NULL);
    *mdata = (unsigned char)(value & 0xFF);
    mdata++;
  }
  // ensure the send queue isn't changed while we're traversing it
  pthread_mutex_t *lock = &(client->_midi_send_queue_lock);
  pthread_mutex_lock(lock);
  // if the queue is empty, begin it with the message
  if (client->_midi_send_queue_head == NULL) {
    client->_midi_send_queue_head = message;
  }
  else {
    // insert the message to the client's send queue in whatever position
    //  keeps the queue sorted by time
    Message *last = NULL;
    Message *current = client->_midi_send_queue_head;
    // store the message time to save lookups
    jack_nframes_t mtime = message->time;
    while (current != NULL) {
      // if we find an existing message that comes after this one, insert this
      //  one before it
      if (mtime < current->time) {
        if (last == NULL) {
          client->_midi_send_queue_head = message;
        }
        else {
          last->next = message;
        }
        message->next = current;
        // clear the message to indicate it's been stored in the queue
        message = NULL;
        break;
      }
      // advance to the next message
      last = current;
      current = (Message *)current->next;
    }
    // if we get to the end and the message hasn't been inserted, 
    //  insert it after the last message we have
    if ((message != NULL) && (last != NULL)) {
      last->next = message;
    }
  }
  pthread_mutex_unlock(lock);
  Py_RETURN_NONE;
}

static PyObject *
Port_receive(Port *self) {
  if (! self->_is_mine) {
    _error("Only ports created by jackpatch can receive MIDI messages");
    return(NULL);
  }
  int flags = jack_port_flags(self->_port);
  if ((flags & JackPortIsInput) == 0) {
    _error("Only input ports can receive MIDI messages");
    return(NULL);
  }
  // the client needs to be activated for receiving to work
  Client *client = (Client *)self->client;
  Client_activate(client);
  // skip receiving if the queue is empty
  if (client->_midi_receive_queue_head == NULL) Py_RETURN_NONE;
  // ensure the receive queue isn't changed while we're adding to it
  pthread_mutex_t *lock = &(client->_midi_receive_queue_lock);
  pthread_mutex_lock(lock);
  // get the current sample rate for time conversions
  jack_nframes_t sample_rate = jack_get_sample_rate(client->_client);  
  // pull events from the receive queue for the client
  Message *last = NULL;
  Message *message = client->_midi_receive_queue_head;
  Message *next = NULL;
  jack_port_t *port = self->_port;
  while (message != NULL) {
    // get all messages for this port
    if (message->port == port) {
      // convert the event time from samples to seconds
      double time = (double)message->time / (double)sample_rate;
      // package raw MIDI data into an array
      size_t bytes = message->data_size;
      PyObject *data = PyList_New(bytes);
      unsigned char *c = message->data;
      size_t i;
      for (i = 0; i < bytes; i++) {
        PyList_SET_ITEM(data, i, PyInt_FromLong(*c++));
      }
      PyObject *tuple = Py_BuildValue("(O,d)", data, time);
      Py_DECREF(data);
      // remove the message from the queue once received
      next = (Message *)message->next;
      message->next = NULL;
      if (last != NULL) last->next = next;
      if (message == client->_midi_receive_queue_head) {
        client->_midi_receive_queue_head = next;
      }
      if (message == client->_midi_receive_queue_tail) {
        client->_midi_receive_queue_tail = last;
      }
      free(message);
      // open the queue for changes before we return
      pthread_mutex_unlock(lock);
      // return the message
      return(tuple);
    }
    last = message;
    message = (Message *)message->next;
  }
  pthread_mutex_unlock(lock);
  // if we get here, there were no messages for this port
  Py_RETURN_NONE;
}

// get all ports connected to the given port
static PyObject *
Port_get_connections(Port *self) {
  // make sure we're connected to JACK
  Client *client = (Client *)self->client;
  Client_open(client);
  if (client->_client == NULL) return(NULL);
  // get a list of port names connected to this port
  const char **port_name = jack_port_get_all_connections(
    client->_client, self->_port);
  // convert the port names into a list of Port objects
  PyObject *return_list;
  return_list = PyList_New(0);
  if (port_name != NULL) {
    while (*port_name != NULL) {
      Port *port = (Port *)Port_new(&PortType, NULL, NULL);
      if (port != NULL) {
        PyObject *name = PyString_FromString(*port_name);
        Port_init(port, Py_BuildValue("(O,S)", client, name), Py_BuildValue("{}"));
        Py_XDECREF(name);
        if (PyList_Append(return_list, (PyObject *)port) < 0) {
          _error("Failed to append a port to the list");
          Py_DECREF(return_list);
          return(NULL);
        }
      }
      free((void *)port_name++);
    }
  }
  return(return_list);
}

static PyMemberDef Port_members[] = {
  {"name", T_OBJECT_EX, offsetof(Port, name), READONLY,
   "The port's unique name"},
  {"client", T_OBJECT_EX, offsetof(Port, client), READONLY,
   "The client used to create the port"},
  {"flags", T_OBJECT_EX, offsetof(Port, flags), READONLY,
   "The port's flags as a bitfield of JackPortFlags"},
  {NULL}  /* Sentinel */
};

static PyMethodDef Port_methods[] = {
    {"send", (PyCFunction)Port_send, METH_VARARGS,
      "Send a tuple of ints as a MIDI message to the port"},
    {"receive", (PyCFunction)Port_receive, METH_NOARGS,
      "Receive a MIDI message from the port"},
    {"get_connections", (PyCFunction)Port_get_connections, METH_NOARGS,
      "Get all the ports connected to this one"},
    {NULL, NULL, 0, NULL}  /* Sentinel */
};

static PyTypeObject PortType = {
    PyObject_HEAD_INIT(NULL)
    0,                             /*ob_size*/
    "jackpatch.Port",              /*tp_name*/  
    sizeof(Port),                  /*tp_basicsize*/
    0,                             /*tp_itemsize*/
    (destructor)Port_dealloc,      /*tp_dealloc*/
    0,                             /*tp_print*/
    0,                             /*tp_getattr*/
    0,                             /*tp_setattr*/
    0,                             /*tp_compare*/
    0,                             /*tp_repr*/
    0,                             /*tp_as_number*/
    0,                             /*tp_as_sequence*/
    0,                             /*tp_as_mapping*/
    0,                             /*tp_hash */
    0,                             /*tp_call*/
    0,                             /*tp_str*/
    0,                             /*tp_getattro*/
    0,                             /*tp_setattro*/
    0,                             /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Represents an JACK port",     /* tp_doc */
    0,		                         /* tp_traverse */
    0,		                         /* tp_clear */
    0,		                         /* tp_richcompare */
    0,		                         /* tp_weaklistoffset */
    0,		                         /* tp_iter */
    0,		                         /* tp_iternext */
    Port_methods,                  /* tp_methods */
    Port_members,                  /* tp_members */
    0,                             /* tp_getset */
    0,                             /* tp_base */
    0,                             /* tp_dict */
    0,                             /* tp_descr_get */
    0,                             /* tp_descr_set */
    0,                             /* tp_dictoffset */
    (initproc)Port_init,           /* tp_init */
    0,                             /* tp_alloc */
    Port_new,                      /* tp_new */
};

// MODULE *********************************************************************

static PyMethodDef jackpatch_methods[] = {
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
initjackpatch(void) {
  PyObject* m;

  PortType.tp_new = PyType_GenericNew;
  if (PyType_Ready(&ClientType) < 0)
      return;
  if (PyType_Ready(&TransportType) < 0)
      return;
  if (PyType_Ready(&PortType) < 0)
      return;

  m = Py_InitModule3("jackpatch", jackpatch_methods,
                     "A Pythonic wrapper for the JACK audio connection kit's MIDI and patchbay functionality");

  JackError = PyErr_NewException("jackpatch.JackError", NULL, NULL);
  Py_INCREF(JackError);
  PyModule_AddObject(m, "JackError", JackError);

  // add constants
  PyModule_AddIntConstant(m, "JackPortIsInput", JackPortIsInput);
  PyModule_AddIntConstant(m, "JackPortIsOutput", JackPortIsOutput);
  PyModule_AddIntConstant(m, "JackPortIsPhysical", JackPortIsPhysical);
  PyModule_AddIntConstant(m, "JackPortCanMonitor", JackPortCanMonitor);
  PyModule_AddIntConstant(m, "JackPortIsTerminal", JackPortIsTerminal);

  // add classes
  Py_INCREF(&ClientType);
  PyModule_AddObject(m, "Client", (PyObject *)&ClientType);
  Py_INCREF(&TransportType);
  PyModule_AddObject(m, "Transport", (PyObject *)&TransportType);
  Py_INCREF(&PortType);
  PyModule_AddObject(m, "Port", (PyObject *)&PortType);
}
