
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "../shmem_p2p_if.h"
#include "../shmem_logger.h"
#include "../shmem_barriers_if.h"
#include "shmem_p2p_sysv.h"


int dart_shmem_mkfifo(char *pname) {
  if (mkfifo(pname, 0666) < 0)
    {
      ERROR("Error creating fifo: '%s'\n", pname);
      return DART_ERR_OTHER;
    }
  return 0;
}

int dart_shmem_p2p_init(dart_team_t teamid, size_t tsize,
			dart_unit_t myid, int ikey ) 
{
  int i, slot;
  char buf[128];
  char key[128];
  
  slot = shmem_syncarea_findteam(teamid);
  sprintf(key, "%s-%d", "sysv", ikey);
  
  for (i = 0; i < tsize; i++) {
    team2fifos[slot][i].readfrom    = -1;
    team2fifos[slot][i].writeto     = -1;
    team2fifos[slot][i].pname_read  = 0;
    team2fifos[slot][i].pname_write = 0;
  }
  
  // the unit 'myid' is responsible for creating all named pipes
  // from any other unit to 'myid' ('i'->'myid' for all i)
  for (i = 0; i < tsize; i++)
    {
      // pipe for sending from <i> to <myid>
      sprintf(buf, "/tmp/%s-team-%d-pipe-from-%d-to-%d", 
	      key, teamid, i, myid);
      
      team2fifos[slot][i].pname_read = strdup(buf);
      
      DEBUG("creating this pipe: '%s'", team2fifos[slot][i].pname_read);
      dart_shmem_mkfifo(team2fifos[slot][i].pname_read);
      
      // pipe for sending from <myid> to <i>
      // mkpipe will be called on the receiver side for those
      sprintf(buf, "/tmp/%s-team-%d-pipe-from-%d-to-%d", 
	      key, teamid, myid, i);
      
      team2fifos[slot][i].pname_write = strdup(buf);
    }
  return DART_OK;
}


int dart_shmem_p2p_destroy(dart_team_t teamid, size_t tsize,
			   dart_unit_t myid, int ikey )
{
  int i, slot;
  char *pname;

  DEBUG("dart_shmem_p2p_destroy called with %d %d %d %d\n",
	teamid, tsize, myid, ikey);

  slot = shmem_syncarea_findteam(teamid);  

  for (i = 0; i < tsize; i++)
    {
      if ((pname = team2fifos[slot][i].pname_read))
	{
	  DEBUG("unlinking '%s'", pname);
	  if(pname && unlink(pname) == -1)
	    ERRNO("unlink '%s'", pname);
	  pname=0;
	}
    }
  return DART_OK;
}

int dart_shmem_send(void *buf, size_t nbytes, 
		    dart_team_t teamid, dart_unit_t dest)
{
  int ret, slot;

  slot = shmem_syncarea_findteam(teamid);

  if (team2fifos[slot][dest].writeto < 0)
    {
      ret = team2fifos[slot][dest].writeto = 
	open( team2fifos[slot][dest].pname_write, O_WRONLY);
      if (ret < 0)
	{
	  fprintf(stderr, "Error sending to %d (pipename: '%s') ret=%d\n",
		  dest, team2fifos[slot][dest].pname_write, ret);
	  return -1;
	}
    }
  ret = write(team2fifos[slot][dest].writeto, buf, nbytes);
  return ret;
}

int dart_shmem_recv(void *buf, size_t nbytes,
		    dart_team_t teamid, dart_unit_t source)
{
  int ret, slot;

  slot = shmem_syncarea_findteam(teamid);

  if ((team2fifos[slot][source].readfrom = 
       open( team2fifos[slot][source].pname_read, O_RDONLY)) < 0)
    {
      fprintf(stderr, "Error opening fifo for reading: '%s'\n",
	      team2fifos[slot][source].pname_read);
      return -999;
    }
  
  ret = read(team2fifos[slot][source].readfrom, buf, nbytes);
  return (ret != nbytes) ? -999 : 0;
}
