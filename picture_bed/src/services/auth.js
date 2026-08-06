import { API_CONFIG } from '../config';
import { calculateTextMD5 } from './fileUtils';
import { requestJson } from './http';

export const loginUser = async (username, password) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.LOGIN}`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      user: username,
      pwd: password
    })
  });

  if (data.code !== 0) {
    throw new Error(data.message || '登录失败');
  }
  return data;
};

export const registerUser = async (values) => {
  const data = await requestJson(`${API_CONFIG.BASE_URL}${API_CONFIG.ENDPOINTS.REGISTER}`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      userName: values.username,
      firstPwd: calculateTextMD5(values.password),
      nickName: values.nickname,
      email: values.email,
      phone: values.phone
    })
  });

  if (data.code !== 0 && data.code !== 2 && data.code !== 6) {
    throw new Error(data.message || '注册失败');
  }
  return data;
};
