# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Private credential fixtures: POSIX modes or native Windows owner-only ACLs.

Only use on disposable test paths. These checks do not replace the daemon's
handle-based credential validation or its reparse-point and sharing checks.
"""
import os
import stat
import subprocess


def _windows_permissions(path, operation):
    # Fixed script and literal paths: fixture names never become PowerShell code.
    script = r"""
$ErrorActionPreference = 'Stop'
$item = Get-Item -LiteralPath $env:OLC_TEST_PERMISSION_PATH -Force
if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point fixture' }
$user = [Security.Principal.WindowsIdentity]::GetCurrent().User
if ($env:OLC_TEST_PERMISSION_OPERATION -ne 'check') {
    if ($item.PSIsContainer) { $acl = New-Object Security.AccessControl.DirectorySecurity }
    else { $acl = New-Object Security.AccessControl.FileSecurity }
    $acl.SetOwner($user)
    $acl.SetAccessRuleProtection($true, $false)
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($user, 'FullControl', 'Allow'))
    if ($env:OLC_TEST_PERMISSION_OPERATION -eq 'public') {
        $everyone = [Security.Principal.SecurityIdentifier]::new('S-1-1-0')
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($everyone, 'Read', 'Allow'))
    }
    Set-Acl -LiteralPath $item.FullName -AclObject $acl
} else {
    $acl = Get-Acl -LiteralPath $item.FullName
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]) -ne $user -or
        -not $acl.AreAccessRulesProtected) { throw 'Fixture is not protected and owner-only' }
    $rules = $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])
    if ($rules.Count -eq 0) { throw 'Empty fixture ACL' }
    foreach ($rule in $rules) {
        if ($rule.IdentityReference -ne $user -or $rule.AccessControlType -ne 'Allow' -or
            $rule.IsInherited -or
            ($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly)) {
            throw 'Non-private fixture ACL'
        }
    }
}
"""
    subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                   env=dict(os.environ, OLC_TEST_PERMISSION_PATH=str(path.absolute()),
                            OLC_TEST_PERMISSION_OPERATION=operation), check=True)


def set_credential_permissions(path, mode):
    """400/600/700 are private; 404 deliberately exposes a file to other users."""
    assert mode in (0o400, 0o600, 0o700, 0o404)
    if os.name == "nt":
        _windows_permissions(path, "public" if mode == 0o404 else "private")
    else:
        path.chmod(mode)


def make_private_directory(path):
    path.mkdir(mode=0o700)
    if os.name == "nt":
        set_credential_permissions(path, 0o700)


def assert_private_permissions(path, mode):
    assert mode in (0o400, 0o600, 0o700)
    assert path.is_dir() if mode == 0o700 else path.is_file()
    if os.name == "nt":
        _windows_permissions(path, "check")
    else:
        assert stat.S_IMODE(path.stat().st_mode) == mode
