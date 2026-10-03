// IBUserManagerService.aidl
package top.niunaijun.blackbox.core.system.user;

// Declare any non-default types here with import statements
import top.niunaijun.blackbox.core.system.user.BUserInfo;
import java.util.List;


interface IBUserManagerService {
    BUserInfo getUserInfo(int userId);
    boolean exists(int userId);
    BUserInfo createUser(int userId);
    boolean renameUser(int userId, String name);
    List<BUserInfo> getUsers();
    void deleteUser(int userId);
}
