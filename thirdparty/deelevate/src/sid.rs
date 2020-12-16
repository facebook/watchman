use std::io::{Error as IoError, Result as IoResult};
use winapi::shared::minwindef::DWORD;
use winapi::shared::winerror::ERROR_INSUFFICIENT_BUFFER;
use winapi::um::errhandlingapi::GetLastError;
use winapi::um::securitybaseapi::{CreateWellKnownSid, GetLengthSid, IsWellKnownSid};
use winapi::um::winnt::SID;
use winapi::um::winnt::WELL_KNOWN_SID_TYPE;

/// A little helper trait to make it easier to operate on SIDs
/// that reside in various storage
pub trait AsSid {
    fn as_sid(self) -> *const SID;
}

impl AsSid for *const SID {
    fn as_sid(self) -> *const SID {
        self
    }
}

/// Return true if sid matches the requested well known sid type
pub fn is_well_known<S: AsSid>(sid: S, sid_type: WELL_KNOWN_SID_TYPE) -> bool {
    let result = unsafe { IsWellKnownSid(sid.as_sid() as *mut _, sid_type) };
    result == 1
}

pub fn get_length_sid<S: AsSid>(sid: S) -> DWORD {
    unsafe { GetLengthSid(sid.as_sid() as *mut _) }
}

/// Stores the data for a well known sid instance
pub struct WellKnownSid {
    data: Vec<u8>,
}

impl WellKnownSid {
    /// Construct a Sid from a well known sid identifier
    pub fn with_well_known(sid_type: WELL_KNOWN_SID_TYPE) -> IoResult<Self> {
        // Measure storage requirements by doing a dry run with no buffer
        let mut size: DWORD = 0;
        let err;
        unsafe {
            CreateWellKnownSid(
                sid_type,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                &mut size,
            );
            err = GetLastError();
        };
        // The call should have failed and told us we need more space
        if err != ERROR_INSUFFICIENT_BUFFER {
            return Err(IoError::last_os_error());
        }

        // Allocate and zero out the storage
        let mut data = vec![0u8; size as usize];

        // and now populate the sid for real
        unsafe {
            if CreateWellKnownSid(
                sid_type,
                std::ptr::null_mut(),
                data.as_mut_ptr() as _,
                &mut size,
            ) == 0
            {
                return Err(IoError::last_os_error());
            }
        }

        Ok(Self { data })
    }
}

impl AsSid for &WellKnownSid {
    fn as_sid(self) -> *const SID {
        self.data.as_ptr() as *const SID
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use winapi::um::winnt::{WinBuiltinAdministratorsSid, WinBuiltinUsersSid};

    #[test]
    fn sid_well_known() {
        let sid = WellKnownSid::with_well_known(WinBuiltinAdministratorsSid).unwrap();
        assert!(is_well_known(&sid, WinBuiltinAdministratorsSid));
        assert!(!is_well_known(&sid, WinBuiltinUsersSid));
    }
}
