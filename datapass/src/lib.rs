use libc::{c_char, c_void, c_void, fd_t, mmap, off_t, open, size_t, write};
use std::collections::HashMap;
use std::error::Error;
use std::os::unix::net::UnixStream;

struct DPMessage {
    size: size_t,
    fd: fd_t,
}

enum PState {
    INIT,
    CLOSED,
    OPEN,
}

trait CObj {
    fn fd() -> fd_t;
    fn setcache(level: CLevel);
    fn set(u128, Vec<u8>)) -> Result;
}


struct TObj {
    size: size_t,
    obj: Vec<u8>,
}

struct RObj {
    size: size_t,
    obj: Vec<u8>,
}

impl CObj for TObj{
    
} 

struct FdPipeConn {
    state: PState,
    connfd: std::os::unix::net::UnixStream,
}

struct DPConnection {
    server: bool,
    conntype: String,
    conn: Option<FdPipeConn>,
}

impl DPConnection {
    pub fn copy_send(&self, source: usize) -> bool {
        true
    }

    pub fn get_thing(&self, id: &u128) -> Option<RObj> {
        if !self.server {
            Some(RObj {
                size: 0,
                obj: Vec::new(),
            })
        } else {
            None
        }
    }

    pub fn setup_server(&self, conf: &std::collections::HashMap<String, String>) -> &DPConnection {
        self
    }

    pub fn setup_client(&self, conf: &std::collections::HashMap<String, String>) -> &DPConnection {
        self
    }
}
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_init() {
        ()
    }
}
