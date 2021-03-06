void init_queue(queue *Q) {
  Q->head = NULL;
  Q->tail = NULL;
  Q->active_jobs = 0;
  pthread_mutex_init(&(Q->active_jobs_lock), NULL);
  pthread_mutex_init(&(Q->head_lock), NULL); 
  pthread_mutex_init(&(Q->tail_lock), NULL);
}

job *get_local_job() { //dequeue local job
  job_list_node *current_node;
  current_node = my_queue->head;
  while (current_node && current_node->entry && current_node->entry->status != READY) {
    current_node = current_node->next;
  }
  if(current_node && current_node->entry) {
    pthread_mutex_lock(&(current_node->lock));
    if(current_node->entry->status == READY) {
      current_node->entry->status = RUNNING;
      pthread_mutex_unlock(&(current_node->lock));
      update_job_count(my_queue, -1);
      return current_node->entry;
    }
    pthread_mutex_unlock(&(current_node->lock));
  }
  return NULL;
}

void print_job_queue(queue *Q) {
  job_list_node *runner;
  runner = Q->head;
  printf("(%d active):\n", Q->active_jobs);
    printf("%-10s   %-5s   %-5s\n",
	   "name", "id", "hash");
  while(runner) {
    printf("%-10s | %-5d | %-5d", runner->entry->name, runner->entry->id,
	   hash(runner->entry->name, runner->entry->id));
    if(runner == Q->head) {
      printf(" (Head)");
    }
    if(runner == Q->tail) {
      printf(" (Tail)");
    }
    printf("\n");
    runner = runner->next;
  }
}

int redistribute_jobs(queue *Q) {
#ifdef GREEDY
  return 0;
#endif
  int count = 0;
  job_list_node *prev, *runner = Q->head;
  host_list_node *dest;
  while(runner != NULL) {
#ifdef VERBOSE2
    print_job_queue(Q);
#endif
    prev = runner;
    runner = runner->next;
    dest = determine_ownership(prev->entry);
    if(dest != my_host) {
      remove_job(prev, my_queue);
      transfer_job(dest->host, prev->entry);
      
      if(prev->entry->status == READY) {
	count++;
	update_job_count(Q, -1);
      } 
      free_job_node(prev);
    }
  }
  return count;
}

void update_q_host_failed () {
  job_list_node *prev, *found, *runner = backup_queue->head;
  host_list_node *dest;
  job *temp;
  while(runner != NULL) {
    prev = runner;
    runner = runner->next;
    dest = determine_ownership(prev->entry);
    if(dest == my_host) {
      remove_job(prev, backup_queue);
      if(contains(prev->entry->id, my_queue)) {
	free_job_node(prev);
      } else {
	add_node_to_queue(prev, my_queue);
      }
    }
  }
}

void free_queue(queue *Q) {
  job_list_node *runner = Q->head;
  if(runner) {
    while(runner->next) {
      if(runner->prev) {
	free_job_node(runner->prev);
      }
      runner = runner->next;    
    }
    free_job_node(runner);
  }
  pthread_mutex_destroy(&(Q->active_jobs_lock));
  pthread_mutex_destroy(&(Q->head_lock));
  pthread_mutex_destroy(&(Q->tail_lock));
  free(Q);
}

void free_job_node(job_list_node *item) {
  free(item->entry);
  pthread_mutex_destroy(&(item->lock));
  free(item);
}

int remove_job(job_list_node *item, queue *list) {

  if(item->prev) {
    pthread_mutex_lock(&(item->prev->lock));
  } else {
    pthread_mutex_lock(&(list->head_lock));
  }

  pthread_mutex_lock(&(item->lock));

  if(item->next) {
    pthread_mutex_lock(&(item->next->lock));
  } else {
    pthread_mutex_lock(&(list->tail_lock));
  }

  if(item->prev) {
    item->prev->next = item->next;
  } else {
    list->head = item->next; 
  }

  if(item->next) {
    item->next->prev = item->prev;
  } else {
    list->tail = item->prev;
  }

  if(item->prev) {
    pthread_mutex_unlock(&(item->prev->lock));
  } else {
    pthread_mutex_unlock(&(list->head_lock));
  }

  pthread_mutex_unlock(&(item->lock));

  if(item->next) {
    pthread_mutex_unlock(&(item->next->lock));
  } else {
    pthread_mutex_unlock(&(list->tail_lock));
  }
  update_job_count(list, -1);
}

int transfer_job(host_port *host, job *to_send) {
  int connection, err;
  err = make_connection_with(host->ip, host->port, &connection);
  if (err < OKAY) {
    handle_failure(host, 1);
    return FAILURE;
  }
  err = TRANSFER_JOB;
  do_rpc(&err);
  err = safe_send(connection, to_send, sizeof(job));
  if (err < OKAY) {
    problem("Transfer Job failed\n");
    return FAILURE;
  }
  close(connection);
  return 0;
}

void replicate_my_jobs() {
  job_list_node *runner = my_queue->head;
  while(runner) {
    replicate_job(runner->entry);
    runner = runner->next;
  }
}

int replicate_job(job *to_send) {
  int connection, err;
  err = make_connection_with(my_host->next->host->ip, my_host->next->host->port, &connection);
  if (err < OKAY) {
    handle_failure(my_host->next->host, 1);
    return FAILURE;
  }
  err = RECEIVE_JOB_COPY;
  do_rpc(&err);
  err = safe_send(connection, to_send, sizeof(job));
  if (err < OKAY) {
    problem("Replicate Job failed\n");
    return FAILURE;
  }
  return 0;
}

int send_meta_data(job *ajob) {
  ajob->status = READY; //needs to be changed once we add dependencies
  host_list_node *dest = determine_ownership(ajob);
#ifdef GREEDY
  add_to_active_queue(ajob);
#else
  if(dest == my_host) {
    add_to_queue(ajob, my_queue);
    if(my_host->next != my_host)
      replicate_job(ajob);
  } else {
    transfer_job(dest->host, ajob);
    free(ajob);
  }
#endif
}

host_list_node *determine_ownership(job *ajob) {
  char buffer[BUFFER_SIZE];
  int job_hash;
  host_list_node *runner;
  job_hash = hash(ajob->name, ajob->id);
#ifdef VERBOSE2
  printf("%s, %d hashes to %d\n", ajob->name, ajob->id, job_hash);
#endif
  runner = server_list->head;
  do {
    runner = runner->next;
  } while(runner->host->location <= job_hash && runner->host->location != 0);
#ifdef VERBOSE2
  printf("So it belongs to %s\n", runner->host->ip);
#endif
  return runner;
}

int write_files(job *ajob, int num_files, data_size *files) {
  char buffer[BUFFER_SIZE], back[BUFFER_SIZE];
  FILE *temp;
  int i;
  sprintf(buffer,"./jobs/%d/", ajob->id); 
  if(mkdir(buffer, S_IRWXU)) {
    if(errno == EEXIST) {
#ifdef VERBOSE2
      problem("directory already exists...\n");
#endif
    } else {
      problem("mkdir failed with %d\n", errno);
    }
  }
  for(i = 0; i < num_files; i++) {
    printf("%s\n", files[i].name); 
    sprintf(buffer,"jobs/%d/%s", ajob->id, files[i].name);
    temp = NULL;
    temp = fopen(buffer, "w");
    if(temp) {
      fwrite(files[i].data, files[i].size, 1, temp);
    } else {
      problem("failed to open file %s, errno: %d\n", files[i].name, errno);
    }
    fclose(temp);

    //
    char mode[] = "0777";
    int i;
    i = strtol(mode, 0, 8);
    if (chmod (buffer,i) < 0) {
	problem("chmod failed");
      }
  }
}

job *get_job_for_runner(){
#ifdef NO_DEQUEUE
  return NULL;
#endif
  job *to_return;
#ifdef RUN_LOCAL
  if(to_return = get_local_job()) {
    return to_return;
  }
#endif
  if(to_return = get_remote_job()) {
    return to_return;
  }
  return NULL;
}

job *get_remote_job() {
  int err;
  int status;
  int connection;
  host_port *server;
  job *location = NULL;
  server = find_job_server();
  if(!server || server == my_host->host) {
    //find_job_server only fails when no one has any jobs
    return NULL;
  }
  err = make_connection_with(server->ip, server->port, &connection);
  if (err < OKAY) { 
    handle_failure(server, 1);
    return NULL;
  }
  err = SERVE_JOB;
  do_rpc(&err);

  status = FAILURE;
  err = safe_recv(connection, &status, sizeof(int)); //find out if they have a job
  if(err < OKAY) {
    close(connection);
    return NULL;
  }
  if(status < 0) {
    //set stuff up so we can try again because they didnt have a job
    server->jobs = 0;
    close(connection);
    return NULL;
  }
  location = malloc(sizeof(job));
  err = safe_recv(connection, location, sizeof(job));
#ifdef VERBOSE2
  printf("Received job %s, %d, from %s\n", (location)->name, (location)->id, server->ip);
#endif
  close(connection);
  return location;
}

host_port *find_job_server() { //Finds the host who we believe to have the most jobs
  host_list_node *n;
  host_port *p;
  n = server_list->head;
  p = n->host;
  do {
    if (p->jobs < n->host->jobs) {
      p = n->host;
    }
    n = n-> next;
  } while(n != server_list->head);
  if(p->jobs) {
    return p;
  } else {
    return NULL;
  }
}

void update_job_count(queue *Q, int update) {
  pthread_mutex_lock(&(Q->active_jobs_lock));
  Q->active_jobs = Q->active_jobs + update;
#ifdef VERBOSE2
  printfl("I have %d jobs", Q->active_jobs);
#endif
  pthread_mutex_unlock(&(Q->active_jobs_lock));
}

void add_to_queue(job *addJob, queue *Q) {
  job_list_node *item = (job_list_node *)malloc(sizeof(job_list_node));
  pthread_mutex_init(&(item->lock), NULL);
  item->next = NULL;
  item->entry = addJob;
  add_node_to_queue(item, Q);
}

job_list_node *contains(unsigned int id, queue *Q) { //returns the node with its lock acquired
  //threadsafe
  //may not need to be threadsafe
  job_list_node *runner;
  pthread_mutex_t *last_lock;
  last_lock = (&(Q->head_lock));
  pthread_mutex_lock(last_lock);
  runner = Q->head;
  while(runner) {
    pthread_mutex_lock(&(runner->lock));
    if(runner->entry->id == id) {
      pthread_mutex_unlock(last_lock);
      return runner;
    }
    pthread_mutex_unlock(last_lock);
    last_lock = &(runner->lock);
    runner = runner->next;
  }
  pthread_mutex_unlock(last_lock);
  return NULL;
}

void add_node_to_queue(job_list_node *item, queue *Q) {
  pthread_mutex_lock(&(item->lock));
  pthread_mutex_lock(&(Q->tail_lock)); //We only need to acquire this lock because we only want to prevent other
  //additions and removals, i.e. we need the tail to remain who it is until we are done.
  if(Q->tail) {
    item->prev = Q->tail;
    Q->tail->next = item;
    Q->tail = item;
  } else {
    item->prev = NULL;
    Q->tail = item;
    //Again, it may seem that we want to acquire the head_lock, but we don't need to since the only place the
    //head will be changed when the list is empty is with an addition
    Q->head = item;
  }
  pthread_mutex_unlock(&(Q->tail_lock));
  pthread_mutex_unlock(&(item->lock));
  update_job_count(Q, 1);
}




//Not currently in use






int inform_of_completion(job *completed) {
  return OKAY;
}
